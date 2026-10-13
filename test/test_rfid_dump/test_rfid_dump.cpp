/**
 * @file test_rfid_dump.cpp
 * @brief Unit tests for the pure .nfc parser + write-plan builder (slice-0043).
 *
 * The plan builder is the heart of RFID Write's safety: block 0, sector trailers, and
 * unreadable ("??") blocks must never land in the data-write set. A naive "write every parsed
 * block" implementation fails test_plan_excludes_block0_trailer_unknown — that is the required
 * fails-before/passes-after regression.
 */

#include <unity.h>
#include "hal/rfid/rfid_dump.h"
#include <string>

using namespace adversary;

// A Version-4 streamed dump covering sectors 0 and 1: block 0 (manufacturer), a known data
// block, an unreadable ("??") data block, and the two sector trailers (blocks 3 and 7).
static const char* kDump =
    "Filetype: Flipper NFC device\n"
    "Version: 4\n"
    "Device type: MIFARE Classic\n"
    "UID: C2 D1 9C 1E\n"
    "ATQA: 00 04\n"
    "SAK: 08\n"
    "Mifare Classic type: 1K\n"
    "Block 0: C2 D1 9C 1E 65 88 04 00 62 63 64 65 66 67 68 69\n"
    "Block 1: 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11\n"
    "Block 2: ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ??\n"
    "Block 3: 00 00 00 00 00 00 FF 07 80 69 FF FF FF FF FF FF\n"
    "Key A: FF FF FF FF FF FF\n"
    "Key B: FF FF FF FF FF FF\n"
    "\n"
    "Block 4: AB CD EF 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D\n"
    "Block 5: 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55 55\n"
    "Block 6: 66 66 66 66 66 66 66 66 66 66 66 66 66 66 66 66\n"
    "Block 7: 00 00 00 00 00 00 FF 07 80 69 FF FF FF FF FF FF\n"
    "Key A: A0 A1 A2 A3 A4 A5\n"
    "Key B: FF FF FF FF FF FF\n";

void setUp() {}
void tearDown() {}

void test_parse_valid_dump() {
    NfcDump dump = parseNfcDump(kDump);

    TEST_ASSERT_TRUE(dump.valid);
    TEST_ASSERT_EQUAL_STRING("C2D19C1E", dump.uid.c_str());
    TEST_ASSERT_EQUAL_UINT8(0x08, dump.sak);
    TEST_ASSERT_EQUAL_UINT32(8, dump.blocks.size());

    // Sector 0 keys both FF; sector 1 Key A is A0..A5.
    TEST_ASSERT_TRUE(dump.keys[0].keyAKnown);
    TEST_ASSERT_EQUAL_UINT8(0xFF, dump.keys[0].keyA[0]);
    TEST_ASSERT_TRUE(dump.keys[1].keyAKnown);
    TEST_ASSERT_EQUAL_UINT8(0xA0, dump.keys[1].keyA[0]);
    TEST_ASSERT_EQUAL_UINT8(0xA5, dump.keys[1].keyA[5]);
}

// The fails-before/passes-after regression: block 0, trailers (3,7), and the "??" block (2)
// are excluded from data writes; only readable non-trailer data blocks remain.
void test_plan_excludes_block0_trailer_unknown() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);

    // Known data blocks are 1, 4, 5, 6 (not 0, not trailers 3/7, not the ?? block 2).
    TEST_ASSERT_EQUAL_UINT32(4, plan.dataWrites.size());
    for (const auto& item : plan.dataWrites) {
        TEST_ASSERT_NOT_EQUAL(0, item.addr);            // never block 0
        TEST_ASSERT_NOT_EQUAL(3, item.addr % 4);        // never a sector trailer
        TEST_ASSERT_NOT_EQUAL(2, item.addr);            // never the unreadable block
    }

    TEST_ASSERT_EQUAL_INT(1, plan.skippedUnknown);      // block 2
    TEST_ASSERT_EQUAL_INT(2, plan.skippedTrailer);      // blocks 3 and 7

    // Block 0 is captured as the manufacturer block (for the opt-in UID write), not a data write.
    TEST_ASSERT_TRUE(plan.hasManufacturer);
    TEST_ASSERT_EQUAL_UINT8(0, plan.manufacturer.addr);
    TEST_ASSERT_EQUAL_UINT8(0xC2, plan.manufacturer.bytes[0]);
}

void test_plan_preserves_block_bytes() {
    NfcDump dump = parseNfcDump(kDump);
    WritePlan plan = buildWritePlan(dump);

    const WritePlanItem* block4 = nullptr;
    for (const auto& item : plan.dataWrites) {
        if (item.addr == 4) block4 = &item;
    }
    TEST_ASSERT_NOT_NULL(block4);
    TEST_ASSERT_EQUAL_UINT8(1, block4->sector);         // block 4 is in sector 1
    TEST_ASSERT_EQUAL_UINT8(0xAB, block4->bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0x0D, block4->bytes[15]);
}

void test_parse_fails_loud_on_malformed_hex() {
    std::string bad =
        "Version: 4\n"
        "UID: C2 D1 9C 1E\n"
        "SAK: 08\n"
        "Block 1: ZZ 11 11 11 11 11 11 11 11 11 11 11 11 11 11 11\n";
    NfcDump dump = parseNfcDump(bad);

    TEST_ASSERT_FALSE(dump.valid);
    TEST_ASSERT_TRUE(dump.parseError.length() > 0);
}

// A dump saved with the audit's "Read Block Data" option off carries keys but no Block lines,
// so there is nothing to write — the UI reports this rather than writing an empty card.
void test_keys_only_dump_has_no_datawrites() {
    std::string keysOnly =
        "Filetype: Flipper NFC device\n"
        "Version: 3\n"
        "Device type: Mifare Classic 1K\n"
        "# Sector 0\n"
        "Key A: FF FF FF FF FF FF\n"
        "Key B: FF FF FF FF FF FF\n"
        "# Sector 1\n"
        "Key A: A0 A1 A2 A3 A4 A5\n";
    NfcDump dump = parseNfcDump(keysOnly);

    TEST_ASSERT_TRUE(dump.valid);
    TEST_ASSERT_EQUAL_UINT32(0, dump.blocks.size());
    WritePlan plan = buildWritePlan(dump);
    TEST_ASSERT_EQUAL_UINT32(0, plan.dataWrites.size());
    TEST_ASSERT_FALSE(plan.hasManufacturer);
    // Keys still parsed onto their sectors via the "# Sector N" anchors.
    TEST_ASSERT_TRUE(dump.keys[1].keyAKnown);
    TEST_ASSERT_EQUAL_UINT8(0xA0, dump.keys[1].keyA[0]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_parse_valid_dump);
    RUN_TEST(test_plan_excludes_block0_trailer_unknown);
    RUN_TEST(test_plan_preserves_block_bytes);
    RUN_TEST(test_parse_fails_loud_on_malformed_hex);
    RUN_TEST(test_keys_only_dump_has_no_datawrites);
    return UNITY_END();
}
