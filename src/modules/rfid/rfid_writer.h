/**
 * @file rfid_writer.h
 * @brief Writes a parsed .nfc dump back onto a MIFARE Classic card (slice-0043).
 *
 * Sibling to RfidAudit: a thin layer over MFRC522_I2C that owns only card selection, per-sector
 * authentication, and MIFARE_Write. All block classification and safety decisions live in the
 * pure rfid_dump layer; this class just executes the plan and reports every outcome.
 */

#pragma once

#include <Arduino.h>
#include <MFRC522_I2C.h>
#include <functional>
#include <string>

#include "rfid_dump.h"

namespace adversary {

/// Per-block progress for the UI while a restore runs.
struct WriteProgress {
    int done;
    int total;
    uint8_t currentBlock;
};

/**
 * @brief Outcome of a restore. Every count is reported (never swallowed): a locked sector or a
 * NAK'd block is an expected negative the operator must see, not an error to mask.
 */
struct WriteResult {
    bool cardSelected = false;  ///< false means no card answered — nothing was attempted.
    int totalPlanned = 0;
    int blocksWritten = 0;
    int failedAuth = 0;         ///< Data blocks whose sector would not authenticate.
    int failedWrite = 0;        ///< Data blocks that authenticated but NAK'd on write.
    bool uidAttempted = false;
    bool uidWritten = false;
    std::string uidResult;      ///< Verbatim outcome of the block-0 attempt (Gen2/CUID verdict).
};

class RfidWriter {
public:
    explicit RfidWriter(MFRC522_I2C* rfid);

    /**
     * @brief Restore a write plan onto the card currently on the reader.
     * @param includeUid When true, additionally attempt a plain MIFARE_Write to block 0 — which
     *        succeeds only on a Gen2/CUID direct-write card and is reported as the definitive
     *        Gen2/CUID verdict. Fails loud (NAK reported) on an ordinary card's locked block 0.
     */
    WriteResult restore(const WritePlan& plan, const NfcDump& dump, bool includeUid,
                        std::function<void(const WriteProgress&)> onProgress = nullptr);

private:
    MFRC522_I2C* m_rfid;

    /// Wake + select the card, mirroring the audit's retry loop. True if a UID was read.
    bool selectCard();

    /// Authenticate `blockAddr`'s sector trying dump Key A, then Key B, then factory FF.
    /// Leaves crypto active on success (so the following MIFARE_Write works).
    bool authBlock(const NfcDump& dump, uint8_t sector, uint8_t blockAddr);

    /// One auth attempt with an explicit key; resets crypto first, re-selects on failure.
    bool tryAuth(uint8_t command, uint8_t blockAddr, const uint8_t* key);
};

} // namespace adversary
