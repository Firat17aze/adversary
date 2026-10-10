/**
 * @file test_rfid_writer.cpp
 * @brief Unit tests for RfidWriter against the recording MFRC522 mock (slice-0043).
 *
 * Covers the write path's observable contract: planned data blocks are written after auth; the
 * factory-FF fallback covers a sector whose dump key is absent; a sector that will not
 * authenticate is counted (failedAuth), never silently skipped; and the opt-in UID write reports
 * a locked block 0 as the Gen2/CUID verdict without aborting the data writes.
 */

#include <unity.h>
#include "modules/rfid/rfid_writer.h"
#include "modules/rfid/rfid_dump.h"
#include <string>

using namespace adversary;

// Two full sectors, all blocks readable, sector keys all FF (which the mock authenticates).
static const char* kDump =
    "Version: 4\n"
    "UID: C2 D1 9C 1E\n"
    "SAK: 08\n"
    "Block 0: C2 D1 9C 1E 65 88 04 00 62 63 64 65 66 67 68 69\n"
    "Block 1: 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11\n"
    "Block 2: 22 22 22 22 22 22 22 22 22 22 22 22 22 22 22 22\n"
    "Block 3: 00 00 00 00 00 00 FF 07 80 69 FF FF FF FF FF FF\n"
    "Key A: FF FF FF FF FF FF\n"
    "Key B: FF FF FF FF FF FF\n"
    "Block 4: AB CD EF 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D\n"
    "Block 5: 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55\n"
    "Block 6: 66 66 66 66 66 66 66 66 66 66 66 66 66 66 66 66\n"
    "Block 7: 00 00 00 00 00 00 FF 07 80 69 FF FF FF FF FF FF\n"
    "Key A: FF FF FF FF FF FF\n"
    "Key B: FF FF FF FF FF FF\n";

// Same blocks, but sector 1 carries no key lines — the writer must reach it via the FF fallback.
static const char* kDumpNoSector1Key =
    "Version: 4\n"
    "UID: C2 D1 9C 1E\n"
    "SAK: 08\n"
    "Block 0: C2 D1 9C 1E 65 88 04 00 62 63 64 65 66 67 68 69\n"
    "Block 1: 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11\n"
    "Block 3: 00 00 00 00 00 00 FF 07 80 69 FF FF FF FF FF FF\n"
    "Key A: FF FF FF FF FF FF\n"
    "Block 4: AB CD EF 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D\n"
    "Block 5: 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55\n"
    "Block 6: 66 66 66 66 66 66 66 66 66 66 66 66 66 66 66 66\n"
    "Block 7: 00 00 00 00 00 00 FF 07 80 69 FF FF FF FF FF FF\n";

void setUp() {
    g_mockWrites.clear();
    g_mockBlock0Nak = false;
    g_mockAuthFails = false;
    g_mockMfrcVersion = 0x92;
}
void tearDown() {}

void test_writer_writes_all_planned_data_blocks() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);  // data blocks 1,2,4,5,6
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/false);

    TEST_ASSERT_TRUE(result.cardSelected);
    TEST_ASSERT_EQUAL_INT((int)plan.dataWrites.size(), result.blocksWritten);
    TEST_ASSERT_EQUAL_INT(0, result.failedAuth);
    TEST_ASSERT_EQUAL_INT(0, result.failedWrite);

    // Exactly the data blocks were written; block 0 was not touched (UID off).
    TEST_ASSERT_EQUAL_UINT32(plan.dataWrites.size(), g_mockWrites.size());
    for (const auto& w : g_mockWrites) {
        TEST_ASSERT_NOT_EQUAL(0, w.addr);
    }
}

void test_writer_ff_fallback_writes_when_key_absent() {
    NfcDump dump = parseNfcDump(kDumpNoSector1Key);
    WritePlan plan = buildWritePlan(dump);
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/false);

    // Sector 1 (blocks 4,5,6) had no dump key, yet still writes via the factory-FF fallback.
    TEST_ASSERT_EQUAL_INT((int)plan.dataWrites.size(), result.blocksWritten);
    TEST_ASSERT_EQUAL_INT(0, result.failedAuth);
}

void test_writer_failed_auth_is_counted_not_written() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);
    g_mockAuthFails = true;  // no key (not even FF) authenticates

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/false);

    TEST_ASSERT_TRUE(result.cardSelected);  // selection does not depend on auth
    TEST_ASSERT_EQUAL_INT((int)plan.dataWrites.size(), result.failedAuth);
    TEST_ASSERT_EQUAL_INT(0, result.blocksWritten);
    TEST_ASSERT_EQUAL_UINT32(0, g_mockWrites.size());  // nothing written without auth
}

// DoD scenario 3: an ordinary card's locked block 0 NAKs; it is reported as the Gen2/CUID
// verdict and the data writes still complete — the locked UID is an expected negative.
void test_writer_uid_locked_reports_and_does_not_abort() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);
    g_mockBlock0Nak = true;

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/true);

    TEST_ASSERT_TRUE(result.uidAttempted);
    TEST_ASSERT_FALSE(result.uidWritten);
    TEST_ASSERT_TRUE(result.uidResult.find("locked") != std::string::npos);
    // Data writes completed despite the UID failure.
    TEST_ASSERT_EQUAL_INT((int)plan.dataWrites.size(), result.blocksWritten);
    // The block-0 write was actually attempted.
    bool attemptedBlock0 = false;
    for (const auto& w : g_mockWrites) {
        if (w.addr == 0) attemptedBlock0 = true;
    }
    TEST_ASSERT_TRUE(attemptedBlock0);
}

void test_writer_uid_success_reports_gen2() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);
    g_mockBlock0Nak = false;  // block 0 accepts the write => direct-write magic

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/true);

    TEST_ASSERT_TRUE(result.uidAttempted);
    TEST_ASSERT_TRUE(result.uidWritten);
    TEST_ASSERT_TRUE(result.uidResult.find("Gen2") != std::string::npos);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_writer_writes_all_planned_data_blocks);
    RUN_TEST(test_writer_ff_fallback_writes_when_key_absent);
    RUN_TEST(test_writer_failed_auth_is_counted_not_written);
    RUN_TEST(test_writer_uid_locked_reports_and_does_not_abort);
    RUN_TEST(test_writer_uid_success_reports_gen2);
    return UNITY_END();
}
