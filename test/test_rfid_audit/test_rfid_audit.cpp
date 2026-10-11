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

// saveToSD writes to /adversary/captures/RFID/<name>.nfc; the SD mock strips the leading '/' and
// writes relative to the CWD, so dumps land beside the test binary and are removed in tearDown.
static std::string nfcPath(const char* name) {
    return std::string("adversary/captures/RFID/") + name + ".nfc";
}

void setUp() {
    mockSerialClear();
    g_mockAuthFails = false;
    g_mockReadFails = false;
    g_mockReauthNeedsReselect = false;
    g_mockSelectedSinceAuth = true;
    g_mockWupaNeedsHalt = false;
    g_mockTagState = MOCK_TAG_ACTIVE;
}

void tearDown() {
    // Remove any dumps the save-path tests wrote (ignore absence).
    ::remove(nfcPath("blocktest").c_str());
    ::remove(nfcPath("fulldump").c_str());
}

// Read back a saved .nfc dump (the SD mock writes relative to the CWD).
static std::string readNfc(const char* name) {
    FILE* f = fopen(nfcPath(name).c_str(), "r");
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

    audit.runDictionaryAttack([](const AuditProgressInfo&){}, opts);
    TEST_ASSERT_TRUE(audit.saveToSD("blocktest"));

    std::string dump = readNfc("blocktest");
    TEST_ASSERT_TRUE(dump.find("Block 0:") != std::string::npos);  // the dump was actually written
    TEST_ASSERT_TRUE(dump.find("??") == std::string::npos);        // every block read back
}

// slice-0050: the audit buffers block data in RAM (it no longer streams to a file). After a run with
// readData, every cracked sector's blocks must be readable off the result struct. Fails before the
// fix: MifareSectorData had no block storage (the bytes went straight to the dump file and were lost
// to the struct), so blockRead[] did not exist / was never set.
void test_audit_buffers_block_data() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);

    AuditOptions opts;
    opts.readData = true;

    audit.runDictionaryAttack([](const AuditProgressInfo&){}, opts);

    const auto& results = audit.getResults();
    TEST_ASSERT_EQUAL(16, results.size());
    TEST_ASSERT_TRUE(results[0].keyAFound);
    for (int b = 0; b < 4; b++) {
        TEST_ASSERT_TRUE(results[0].blockRead[b]);      // every block was read into the buffer
        TEST_ASSERT_EQUAL_UINT8(0x00, results[0].blocks[b][0]);  // mock reads 16 zero bytes
    }
}

// slice-0050: saveToSD writes the full Flipper-v4 dump (header + per-sector blocks + keys) from the
// buffer to /adversary/captures/RFID/<name>.nfc. Fails before the fix: the old saveToSD wrote a
// keys-only v3 body to the lowercase /rfid/ path, so neither the v4 header, a Block row, nor the
// capital-RFID path are present.
void test_saveToSD_writes_full_flipper_dump() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);

    AuditOptions opts;
    opts.readData = true;

    audit.runDictionaryAttack([](const AuditProgressInfo&){}, opts);
    TEST_ASSERT_TRUE(audit.saveToSD("fulldump"));

    std::string dump = readNfc("fulldump");
    TEST_ASSERT_TRUE(dump.find("Version: 4") != std::string::npos);
    TEST_ASSERT_TRUE(dump.find("Device type: MIFARE Classic") != std::string::npos);
    TEST_ASSERT_TRUE(dump.find("Block 0:") != std::string::npos);  // blocks are in the dump
    TEST_ASSERT_TRUE(dump.find("Key A:") != std::string::npos);    // keys are in the dump
}

// slice-0050: with no cracked keys there is nothing to dump, so saveToSD reports failure loudly
// rather than writing an empty/header-only file.
void test_saveToSD_fails_when_no_results() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);  // never ran an audit -> m_results empty

    TEST_ASSERT_FALSE(audit.saveToSD("fulldump"));
}

// slice-0049: every full-select site must HALT before WUPA. A tag left ACTIVE by the previous
// sector's auth ignores a bare WUPA, so a per-sector re-select that skips PICC_HaltA never wakes the
// tag and the sector's auth fails (composed here with the slice-0046 re-auth desync, which turns an
// unwoken tag into that auth failure). An all-factory-key card then cracks Key A in every sector
// only if the per-sector re-select halts first. Fails before routing that re-select through
// reselectCard (only sector 0's Key A is found -> keysFound == 1); passes after (16).
//
// The count is 16, not the clean-mock 32: Key B recovery leans on tryKey's lightweight per-failure
// WUPA, which slice-0049 deliberately leaves un-helpered (it runs hundreds of times per audit), so
// it stays defeated under this seam. That is fine -- this test pins the per-sector full-select, not
// Key B discovery.
void test_audit_reselect_halts_before_wupa_between_sectors() {
    MFRC522_I2C rfid(0x28, -1);
    RfidAudit audit(&rfid);

    g_mockReauthNeedsReselect = true;  // the real re-auth desync (slice-0046)
    g_mockWupaNeedsHalt = true;        // an ACTIVE tag ignores a bare WUPA (slice-0049)
    g_mockTagState = MOCK_TAG_ACTIVE;  // a card that has been sitting in the field, left ACTIVE

    int keysFound = audit.runDictionaryAttack([](const AuditProgressInfo&){});

    TEST_ASSERT_EQUAL(16, keysFound);  // all 16 sectors' Key A cracked -> every re-select woke the tag
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_audit_dictionary_attack);
    RUN_TEST(test_audit_incompatible_tag);
    RUN_TEST(test_audit_results_clear);
    RUN_TEST(test_audit_reports_block_read_failure);
    RUN_TEST(test_audit_reports_no_key_sector_omitted);
    RUN_TEST(test_audit_reads_all_blocks_under_single_auth);
    RUN_TEST(test_audit_buffers_block_data);
    RUN_TEST(test_saveToSD_writes_full_flipper_dump);
    RUN_TEST(test_saveToSD_fails_when_no_results);
    RUN_TEST(test_audit_reselect_halts_before_wupa_between_sectors);
    return UNITY_END();
}
