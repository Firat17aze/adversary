/**
 * @file rfid_dump.h
 * @brief Pure parser + write-plan builder for Flipper-style .nfc MIFARE Classic dumps.
 *
 * Display- and hardware-free by design: the caller reads the .nfc file into a string, and
 * everything here is plain std::string/std::vector so the parsing and the block-classification
 * logic (where every RFID-Write safety decision lives) is unit-testable off-target. The actual
 * card writes live in rfid_writer.{h,cpp}.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace adversary {

/// MIFARE Classic 1K: 16 sectors x 4 blocks = 64 blocks.
constexpr uint8_t MIFARE_1K_BLOCKS = 64;
constexpr uint8_t MIFARE_BLOCKS_PER_SECTOR = 4;
constexpr uint8_t MIFARE_BLOCK_SIZE = 16;
constexpr uint8_t MIFARE_KEY_SIZE = 6;

/**
 * @brief What a block is, which decides whether it is safe to write in v1.
 *
 * MANUFACTURER (block 0) is the UID/vendor block, locked on ordinary cards (UID opt-in only).
 * SECTOR_TRAILER (every 4th block) holds keys + access bits; a wrong access-bits byte is an
 * irreversible brick, and its key fields read back as zeros, so v1 never writes it (slice-0043).
 */
enum class BlockKind { DATA, SECTOR_TRAILER, MANUFACTURER };

BlockKind classifyBlock(uint8_t addr);

/// One block parsed from a dump. `known` is false for an audit's "?? ?? .." (unreadable) line.
struct NfcBlock {
    uint8_t addr = 0;
    bool known = false;
    uint8_t bytes[MIFARE_BLOCK_SIZE] = {0};
};

/// The cracked keys for one sector, as recorded in the dump's per-sector Key A / Key B lines.
struct NfcSectorKey {
    bool keyAKnown = false;
    uint8_t keyA[MIFARE_KEY_SIZE] = {0};
    bool keyBKnown = false;
    uint8_t keyB[MIFARE_KEY_SIZE] = {0};
};

/**
 * @brief A parsed .nfc dump. `valid` is false (with `parseError` set) on any malformed line —
 * the parser fails loud rather than silently dropping a byte or a block.
 */
struct NfcDump {
    bool valid = false;
    std::string parseError;
    std::string uid;                 ///< Uppercase hex, no separators (e.g. "C2D19C1E").
    uint8_t sak = 0;
    std::vector<NfcBlock> blocks;
    NfcSectorKey keys[16];
};

/// One block the plan will write to the card: address, its sector, and the 16 bytes.
struct WritePlanItem {
    uint8_t addr;
    uint8_t sector;
    uint8_t bytes[MIFARE_BLOCK_SIZE];
};

/**
 * @brief The set of blocks safe to write, plus what was deliberately excluded.
 *
 * `dataWrites` holds only known, non-trailer, non-block-0 blocks. Block 0 is captured in
 * `manufacturer` (written only on the explicit UID opt-in, by the writer). Trailers and
 * unreadable blocks are counted, never written, so the UI can show the operator what it skipped.
 */
struct WritePlan {
    std::vector<WritePlanItem> dataWrites;
    int skippedUnknown = 0;
    int skippedTrailer = 0;
    bool hasManufacturer = false;
    NfcBlock manufacturer;
};

/**
 * @brief Parse a Flipper "Version: 4" MIFARE Classic .nfc dump (as streamed by rfid_audit).
 * @param text The whole file contents.
 * @return A dump with valid==true on success, or valid==false + parseError on a malformed line.
 */
NfcDump parseNfcDump(const std::string& text);

/**
 * @brief Classify every parsed block into the v1 write plan. Pure; block 0 and sector trailers
 *        are never placed in dataWrites regardless of options.
 */
WritePlan buildWritePlan(const NfcDump& dump);

} // namespace adversary
