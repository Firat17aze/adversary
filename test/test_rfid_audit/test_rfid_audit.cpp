/**
 * @file test_rfid_audit.cpp
 * @brief Unit tests for RFID auditing engine
 */

#include <unity.h>
#include <functional>
#include <string>
#include "modules/rfid/rfid_audit.h"

#ifndef ESP32
#include <SD.h>
#include "../common/arduino_mocks.h"
#include "../common/mfrc522_mocks.h"
#endif

using namespace adversary;

// A dump path for the read-path diagnostic tests. The SD mock strips the leading '/' and writes
// relative to the CWD, so this lands beside the test binary and is removed in setUp/tearDown.
static const char* kDiagDumpPath = "/rfid_audit_diag_test.nfc";

void setUp() {
    mockSerialClear();
    g_mockAuthFails = false;
    g_mockReadFails = false;
    g_mockReauthNeedsReselect = false;
    g_mockSelectedSinceAuth = true;
    SD.remove(kDiagDumpPath);
}

void tearDown() {
    SD.remove(kDiagDumpPath);
}

// Read back the streamed dump (the SD mock writes it relative to the CWD).
static std::string readDumpFile() {
    const char* rel = (kDiagDumpPath[0] == '/') ? kDiagDumpPath + 1 : kDiagDumpPath;
    FILE* f = fopen(rel, "r");
    if (!f) return "";
    std::string out;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return out;
}

void test_audit_dictionary_attack() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);
    
    int progressCalls = 0;
    auto onProgress = [&](const AuditProgressInfo& info) {
        progressCalls++;
        // Check if progress is increasing or at least non-negative
        if (progressCalls > 1) {
            TEST_ASSERT_TRUE(info.overallProgress >= -0.001f);
        }
    };
    
    // Our mock returns OK for FF FF FF FF FF FF and A0 A1 A2 A3 A4 A5
    // Both are in the default key list.
    // s=0, try FF FF (Key A index 0) -> Found!
    // s=0, try FF FF (Key B index 0) -> Found!
    // ...
    // Total 16 sectors * 2 keys = 32 keys found.
    
    int result = audit.runDictionaryAttack(onProgress);
    
    TEST_ASSERT_EQUAL(32, result);
    TEST_ASSERT_TRUE(progressCalls > 32);
    
    const auto& results = audit.getResults();
    TEST_ASSERT_EQUAL(16, results.size());
    TEST_ASSERT_TRUE(results[0].keyAFound);
    TEST_ASSERT_TRUE(results[0].keyBFound);
}

void test_audit_incompatible_tag() {
    MFRC522_I2C rfid(0x28, -1);
    // Set SAK to 0x20 (ISO 14443-4, e.g. DESFire)
    rfid.uid.sak = 0x20;
    
    RfidAudit audit(&rfid);
    int result = audit.runDictionaryAttack([](const AuditProgressInfo&){});
    
    // Should return -1 for incompatible type
    TEST_ASSERT_EQUAL(-1, result);
}

void test_audit_results_clear() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);
    
    // First run (compatible tag)
    audit.runDictionaryAttack([](const AuditProgressInfo&){});
    TEST_ASSERT_TRUE(audit.getResults().size() > 0);
    
    // Second run with incompatible tag
    rfid.uid.sak = 0x20; 
    audit.runDictionaryAttack([](const AuditProgressInfo&){});
    
    // Results should be cleared
    TEST_ASSERT_EQUAL(0, audit.getResults().size());
}

// slice-0045: a block that authenticates but will not read back must be attributed to a READ
// failure, distinct from an auth failure -- not left as a silent '??'. Fails before the fix
// (both branches wrote the placeholder with no log, so neither marker exists).
void test_audit_reports_block_read_failure() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);

    g_mockReadFails = true;  // auth (factory FF) succeeds; every block read fails

    AuditOptions opts;
    opts.readData = true;
    opts.dumpPath = kDiagDumpPath;

    audit.runDictionaryAttack([](const AuditProgressInfo&){}, opts);

    TEST_ASSERT_TRUE(g_mockSerialLog.find("READ FAILED") != std::string::npos);
    TEST_ASSERT_TRUE(g_mockSerialLog.find("AUTH FAILED") == std::string::npos);
}

// slice-0045: a sector with no dictionary key is dropped from the dump, so its omission must be
// logged loudly rather than leaving a silent gap. Fails before the fix (only the terse
// "Sector N done: A=N B=N" line existed).
void test_audit_reports_no_key_sector_omitted() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);

    g_mockAuthFails = true;  // no key cracks any sector

    AuditOptions opts;
    opts.readData = true;
    opts.dumpPath = kDiagDumpPath;

    int keysFound = audit.runDictionaryAttack([](const AuditProgressInfo&){}, opts);

    TEST_ASSERT_EQUAL(0, keysFound);
    TEST_ASSERT_TRUE(g_mockSerialLog.find("omitted from dump") != std::string::npos);
}

// slice-0046: one authentication covers a whole sector. With a reader that NAKs a re-auth issued
// without a re-select (the field desync on 23FF9C1E), reading all four blocks under a single
// per-sector auth must leave no '??' in the dump. Fails before the fix (per-block re-auth NAKs
// blocks 2-4 of every sector).
void test_audit_reads_all_blocks_under_single_auth() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);

    g_mockReauthNeedsReselect = true;

    AuditOptions opts;
    opts.readData = true;
    opts.dumpPath = kDiagDumpPath;

    audit.runDictionaryAttack([](const AuditProgressInfo&){}, opts);

    std::string dump = readDumpFile();
    TEST_ASSERT_TRUE(dump.find("Block 0:") != std::string::npos);  // the dump was actually written
    TEST_ASSERT_TRUE(dump.find("??") == std::string::npos);        // every block read back
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_audit_dictionary_attack);
    RUN_TEST(test_audit_incompatible_tag);
    RUN_TEST(test_audit_results_clear);
    RUN_TEST(test_audit_reports_block_read_failure);
    RUN_TEST(test_audit_reports_no_key_sector_omitted);
    RUN_TEST(test_audit_reads_all_blocks_under_single_auth);
    return UNITY_END();
}
