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
    g_mockReauthNeedsReselect = false;
    g_mockSelectedSinceAuth = true;
    g_mockWriteFailFirstN = 0;
    g_mockMagicGen1a = false;   // slice-0048: a test opts a card into Gen1a; others see ordinary
    g_mockBackdoorOpen = false;
    g_mockMagicAckOnAttempt = 0;  // 0 = backdoor verdict follows g_mockMagicGen1a (no flakiness)
    g_mockBackdoorCalls = 0;
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

// DoD scenario 3 (slice-0048 ordinary-card negative): an ordinary card's block 0 NAKs the direct
// write AND does not ACK the backdoor, so both UID strategies fail loud and it is reported as a
// non-magic locked block — while the data writes still complete. g_mockMagicGen1a stays false
// (setUp), so MIFARE_OpenUidBackdoor is refused.
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
    // A NAK'd block-0 write lands nothing, so no block-0 record exists.
    for (const auto& w : g_mockWrites) {
        TEST_ASSERT_NOT_EQUAL(0, w.addr);
    }
    // Data writes completed despite the UID failure.
    TEST_ASSERT_EQUAL_INT((int)plan.dataWrites.size(), result.blocksWritten);
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

// slice-0046: on a reader that NAKs a re-auth without a re-select, a sector whose dump keys are
// absent (factory-only path, a single auth attempt) must still write all its blocks under one
// per-sector authentication. Fails before the fix: the per-block pattern desyncs, so the sector's
// first block fails the lone factory attempt with no fallback and is counted failedAuth.
void test_writer_writes_all_blocks_in_factory_only_sector() {
    NfcDump dump = parseNfcDump(kDumpNoSector1Key);  // sector 1 (blocks 4,5,6) has no key
    WritePlan plan = buildWritePlan(dump);
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);
    g_mockReauthNeedsReselect = true;

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/false);

    TEST_ASSERT_EQUAL_INT((int)plan.dataWrites.size(), result.blocksWritten);
    TEST_ASSERT_EQUAL_INT(0, result.failedAuth);
}

// slice-0047: the first write-back pass after the antenna powers on drops early blocks to a
// transient NAK/timeout (field: 5 blocks lost to status 4, a second pass wrote them all). A
// bounded retry — re-selecting and re-authenticating the sector between attempts — must land every
// block on the first pass. Fails before the fix: the one-shot write records each warm-up miss as a
// permanent failedWrite, so the first two planned blocks never land.
void test_writer_retries_transient_write_nak() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);  // data blocks 1,2,4,5,6
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);
    g_mockWriteFailFirstN = 2;  // first two writes time out (warm-up), then the reader settles

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/false);

    // Every planned block lands despite the two transient misses — none left as a permanent fail.
    TEST_ASSERT_EQUAL_INT((int)plan.dataWrites.size(), result.blocksWritten);
    TEST_ASSERT_EQUAL_INT(0, result.failedWrite);
    TEST_ASSERT_EQUAL_INT(0, result.failedAuth);
}

// slice-0048: a Gen1a card's block 0 NAKs the direct (authenticated) write but is rewritable once
// the backdoor is open. The writer must fall through from the failed direct write to the backdoor,
// land the UID, and report the Gen1a verdict. Fails before the fix: with no backdoor strategy the
// direct NAK ends the step as a locked/non-written block 0.
void test_writer_clones_uid_via_gen1a_backdoor() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);
    g_mockBlock0Nak = true;    // a normal write to block 0 is refused (Gen1a is read-only to it)
    g_mockMagicGen1a = true;   // but the card ACKs the backdoor

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/true);

    TEST_ASSERT_TRUE(result.uidAttempted);
    TEST_ASSERT_TRUE(result.uidWritten);
    TEST_ASSERT_TRUE(result.uidResult.find("Gen1a") != std::string::npos);
    // The backdoor write actually landed block 0.
    bool wroteBlock0 = false;
    for (const auto& w : g_mockWrites) {
        if (w.addr == 0) wroteBlock0 = true;
    }
    TEST_ASSERT_TRUE(wroteBlock0);
    // Data blocks still all wrote.
    TEST_ASSERT_EQUAL_INT((int)plan.dataWrites.size(), result.blocksWritten);
}

// slice-0048 on-device round 1: the Gen1a backdoor open is timing-sensitive and misses on the first
// try (the detector already retries it — slice-0044), so the writer's UID backdoor must retry too or
// a genuinely magic card (23FF9C1E: detected MAGIC: YES, but the single-shot write open failed) is
// misreported as an ordinary locked block. Models a backdoor that only ACKs on the 3rd open: fails
// before the retry loop (single-shot open → "locked", uidWritten false), passes after.
void test_writer_retries_flaky_gen1a_backdoor() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);
    MFRC522_I2C rfid(0x28, -1);
    RfidWriter writer(&rfid);
    g_mockBlock0Nak = true;        // direct write refused
    g_mockMagicAckOnAttempt = 3;   // backdoor ACKs only on the 3rd open attempt

    WriteResult result = writer.restore(plan, dump, /*includeUid=*/true);

    TEST_ASSERT_TRUE(result.uidWritten);
    TEST_ASSERT_TRUE(result.uidResult.find("Gen1a") != std::string::npos);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_writer_writes_all_planned_data_blocks);
    RUN_TEST(test_writer_ff_fallback_writes_when_key_absent);
    RUN_TEST(test_writer_failed_auth_is_counted_not_written);
    RUN_TEST(test_writer_uid_locked_reports_and_does_not_abort);
    RUN_TEST(test_writer_uid_success_reports_gen2);
    RUN_TEST(test_writer_writes_all_blocks_in_factory_only_sector);
    RUN_TEST(test_writer_retries_transient_write_nak);
    RUN_TEST(test_writer_clones_uid_via_gen1a_backdoor);
    RUN_TEST(test_writer_retries_flaky_gen1a_backdoor);
    return UNITY_END();
}
