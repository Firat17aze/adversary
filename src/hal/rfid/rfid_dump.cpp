/**
 * @file rfid_dump.cpp
 * @brief Implementation of the pure .nfc parser + write-plan builder.
 *
 * Deliberately iostream-free: std::istringstream/std::getline pull ~200 KB of C++ locale
 * machinery into the firmware (nothing else in src/ links iostreams), so parsing is done with
 * plain char scanning over std::string.
 */

#include "rfid_dump.h"

#include <cstdio>
#include <cstring>

namespace adversary {

BlockKind classifyBlock(uint8_t addr) {
    if (addr == 0) return BlockKind::MANUFACTURER;
    if (addr % MIFARE_BLOCKS_PER_SECTOR == MIFARE_BLOCKS_PER_SECTOR - 1) {
        return BlockKind::SECTOR_TRAILER;
    }
    return BlockKind::DATA;
}

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// Parse a two-char hex token ("A5") into a byte. False unless it is exactly two hex digits.
bool parseHexByte(const char* token, size_t len, uint8_t& out) {
    if (len != 2) return false;
    int hi = hexDigit(token[0]);
    int lo = hexDigit(token[1]);
    if (hi < 0 || lo < 0) return false;
    out = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

/// Advance `i` past whitespace and yield the next whitespace-delimited token, or false at end.
bool nextToken(const std::string& s, size_t& i, const char*& tokenStart, size_t& tokenLen) {
    size_t n = s.size();
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) i++;
    if (i >= n) return false;
    size_t start = i;
    while (i < n && s[i] != ' ' && s[i] != '\t' && s[i] != '\r') i++;
    tokenStart = s.c_str() + start;
    tokenLen = i - start;
    return true;
}

/**
 * @brief Parse exactly `count` byte tokens from `rest` into `out`.
 *
 * Each token must be two hex digits or the audit's "??" placeholder for an unreadable byte.
 * `allKnown` is set false if any token is "??". Returns false (malformed) if a token is neither
 * form, or fewer than `count` tokens are present — the parser fails loud rather than guessing.
 */
bool parseByteSeq(const std::string& rest, uint8_t* out, int count, bool& allKnown) {
    allKnown = true;
    int parsed = 0;
    size_t i = 0;
    const char* token = nullptr;
    size_t len = 0;
    while (parsed < count && nextToken(rest, i, token, len)) {
        if (len == 2 && token[0] == '?' && token[1] == '?') {
            allKnown = false;
            if (out) out[parsed] = 0;
        } else {
            uint8_t value = 0;
            if (!parseHexByte(token, len, value)) return false;
            if (out) out[parsed] = value;
        }
        parsed++;
    }
    return parsed == count;
}

/// Everything after the first ':' in `line`, left-trimmed; empty if there is no ':'.
std::string afterColon(const std::string& line) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return "";
    size_t start = line.find_first_not_of(" \t", colon + 1);
    return start == std::string::npos ? "" : line.substr(start);
}

std::string leftTrim(const std::string& line) {
    size_t start = line.find_first_not_of(" \t\r");
    return start == std::string::npos ? "" : line.substr(start);
}

bool startsWith(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

} // namespace

NfcDump parseNfcDump(const std::string& text) {
    NfcDump dump;
    int currentSector = -1;  // Set by a "Block N:" or "# Sector N" line; anchors following keys.

    auto fail = [&](const std::string& why) -> NfcDump {
        dump.valid = false;
        dump.parseError = why;
        return dump;
    };

    size_t pos = 0;
    const size_t textLen = text.size();
    while (pos <= textLen) {
        size_t newline = text.find('\n', pos);
        size_t end = (newline == std::string::npos) ? textLen : newline;
        std::string line = leftTrim(text.substr(pos, end - pos));
        if (newline == std::string::npos) {
            pos = textLen + 1;  // Process this final line, then stop.
        } else {
            pos = newline + 1;
        }
        if (line.empty()) continue;

        if (startsWith(line, "# Sector")) {
            int sector = -1;
            if (std::sscanf(line.c_str(), "# Sector %d", &sector) == 1 &&
                sector >= 0 && sector < 16) {
                currentSector = sector;
            }
            continue;
        }
        if (line[0] == '#') continue;  // Other comments.

        if (startsWith(line, "UID:")) {
            std::string rest = afterColon(line);
            dump.uid.clear();
            size_t i = 0;
            const char* token = nullptr;
            size_t len = 0;
            char hex[3];
            while (nextToken(rest, i, token, len)) {
                uint8_t value = 0;
                if (!parseHexByte(token, len, value)) return fail("malformed UID byte");
                std::snprintf(hex, sizeof(hex), "%02X", value);
                dump.uid += hex;
            }
            continue;
        }

        if (startsWith(line, "SAK:")) {
            uint8_t sak = 0;
            bool known = true;
            if (!parseByteSeq(afterColon(line), &sak, 1, known)) {
                return fail("malformed SAK");
            }
            dump.sak = sak;
            continue;
        }

        if (startsWith(line, "Block ")) {
            int addr = -1;
            if (std::sscanf(line.c_str(), "Block %d", &addr) != 1 || addr < 0 ||
                addr >= MIFARE_1K_BLOCKS) {
                return fail("bad block address in: " + line);
            }
            NfcBlock block;
            block.addr = static_cast<uint8_t>(addr);
            bool allKnown = true;
            if (!parseByteSeq(afterColon(line), block.bytes, MIFARE_BLOCK_SIZE, allKnown)) {
                return fail("malformed block data for block " + std::to_string(addr));
            }
            block.known = allKnown;
            currentSector = addr / MIFARE_BLOCKS_PER_SECTOR;
            dump.blocks.push_back(block);
            continue;
        }

        if (startsWith(line, "Key A:") || startsWith(line, "Key B:")) {
            if (currentSector < 0 || currentSector >= 16) {
                return fail("key line with no preceding sector/block");
            }
            bool isKeyA = startsWith(line, "Key A:");
            uint8_t key[MIFARE_KEY_SIZE] = {0};
            bool allKnown = true;
            if (!parseByteSeq(afterColon(line), key, MIFARE_KEY_SIZE, allKnown)) {
                return fail(std::string("malformed ") + (isKeyA ? "Key A" : "Key B"));
            }
            NfcSectorKey& sectorKey = dump.keys[currentSector];
            if (isKeyA) {
                sectorKey.keyAKnown = allKnown;
                std::memcpy(sectorKey.keyA, key, MIFARE_KEY_SIZE);
            } else {
                sectorKey.keyBKnown = allKnown;
                std::memcpy(sectorKey.keyB, key, MIFARE_KEY_SIZE);
            }
            continue;
        }
        // Header lines (Filetype, Version, Device type, ATQA, Mifare Classic type) are ignored.
    }

    dump.valid = true;
    return dump;
}

WritePlan buildWritePlan(const NfcDump& dump) {
    WritePlan plan;
    for (const NfcBlock& block : dump.blocks) {
        switch (classifyBlock(block.addr)) {
            case BlockKind::MANUFACTURER:
                plan.manufacturer = block;
                plan.hasManufacturer = block.known;  // Only writable if we actually read it.
                break;
            case BlockKind::SECTOR_TRAILER:
                plan.skippedTrailer++;
                break;
            case BlockKind::DATA:
                if (block.known) {
                    WritePlanItem item;
                    item.addr = block.addr;
                    item.sector = block.addr / MIFARE_BLOCKS_PER_SECTOR;
                    std::memcpy(item.bytes, block.bytes, MIFARE_BLOCK_SIZE);
                    plan.dataWrites.push_back(item);
                } else {
                    plan.skippedUnknown++;
                }
                break;
        }
    }
    return plan;
}

} // namespace adversary
