/**
 * @file rfid_writer.cpp
 * @brief RfidWriter implementation.
 */

#include "rfid_writer.h"
#include "rfid_reselect.h"
#include "utils/log.h"

#include <cstring>

namespace adversary {

namespace {
constexpr uint8_t FACTORY_KEY[MIFARE_KEY_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// A data-block write that NAKs/times out on the first attempt is usually a transient RF warm-up
// (field: the first write-back after the antenna powers on dropped 5 early blocks to status 4,
// a second pass wrote them all). Retry a bounded number of times, re-selecting and
// re-authenticating the sector between attempts. 3, not the probe/select loops' 5: a write is far
// heavier than a wake, the field miss cleared on the next pass, and a genuinely locked block must
// not stall three extra full re-select+re-auth cycles (slice-0047).
constexpr int WRITE_ATTEMPTS = 3;

// The Gen1a backdoor wake is timing-sensitive: a single MIFARE_OpenUidBackdoor gives false
// negatives (slice-0044 proved the same card reads NO then YES), so a single-shot open misreports a
// genuinely magic card as an ordinary locked block. Retry the open with a re-select between tries,
// matching the detector's MAGIC_PROBE_ATTEMPTS (field: 23FF9C1E detected MAGIC: YES yet the writer's
// single-shot open failed — slice-0048 on-device round 1).
constexpr int BACKDOOR_OPEN_ATTEMPTS = 5;
}

RfidWriter::RfidWriter(MFRC522_I2C* rfid) : m_rfid(rfid) {}

bool RfidWriter::selectCard() {
    return reselectCard(m_rfid);  // the one shared full-select (slice-0049)
}

bool RfidWriter::tryAuth(uint8_t command, uint8_t blockAddr, const uint8_t* key) {
    m_rfid->PCD_StopCrypto1();
    MFRC522_I2C::MIFARE_Key mfKey;
    memcpy(mfKey.keyByte, key, MIFARE_KEY_SIZE);
    MFRC522_I2C::StatusCode status = (MFRC522_I2C::StatusCode)m_rfid->PCD_Authenticate(
        command, blockAddr, &mfKey, &(m_rfid->uid));
    if (status == MFRC522_I2C::STATUS_OK) return true;

    // Failed auth can leave the card unresponsive; reset and re-select before the next attempt,
    // mirroring the audit's recovery path.
    m_rfid->PCD_StopCrypto1();
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);
    m_rfid->PICC_WakeupA(bufferATQA, &bufferSize);
    m_rfid->PICC_ReadCardSerial();
    return false;
}

bool RfidWriter::authBlock(const NfcDump& dump, uint8_t sector, uint8_t blockAddr) {
    const NfcSectorKey& key = dump.keys[sector];
    if (key.keyAKnown && tryAuth(MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A, blockAddr, key.keyA)) {
        return true;
    }
    if (key.keyBKnown && tryAuth(MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_B, blockAddr, key.keyB)) {
        return true;
    }
    // A freshly blanked target still carries factory keys even when the dump's keys do not match.
    return tryAuth(MFRC522_I2C::PICC_CMD_MF_AUTH_KEY_A, blockAddr, FACTORY_KEY);
}

WriteResult RfidWriter::restore(const WritePlan& plan, const NfcDump& dump, bool includeUid,
                                std::function<void(const WriteProgress&)> onProgress) {
    WriteResult result;
    result.totalPlanned =
        static_cast<int>(plan.dataWrites.size()) +
        (includeUid && plan.hasManufacturer ? 1 : 0);

    if (!m_rfid) {
        result.uidResult = "No RFID reader";
        return result;
    }
    if (!selectCard()) {
        LOG_W("RFID", "write: card not responding to WUPA");
        return result;
    }
    result.cardSelected = true;

    int done = 0;
    // One PCD_Authenticate covers a whole sector; re-authenticating per block issues a second auth
    // with no intervening re-select, which the reader NAKs, so every block after a sector's first
    // fails (slice-0046). Writes are addr-ordered, so a sector's blocks are contiguous: authenticate
    // only on a sector change, keep the crypto session open across that sector's blocks, and
    // StopCrypto1 once when the sector changes or the plan ends.
    int authedSector = -1;   // sector the card is currently authenticated to (-1 = none)
    bool sectorAuthed = false;
    for (const WritePlanItem& item : plan.dataWrites) {
        if (item.sector != authedSector) {
            if (sectorAuthed) m_rfid->PCD_StopCrypto1();  // close the previous sector's session
            authedSector = item.sector;
            selectCard();
            sectorAuthed = authBlock(dump, item.sector, item.addr);
            if (!sectorAuthed) {
                LOG_W("RFID", "write: sector %d auth failed", item.sector);
            }
        }

        if (!sectorAuthed) {
            result.failedAuth++;
            LOG_D("RFID", "write: block %d skipped (sector %d not authenticated)",
                  item.addr, item.sector);
        } else {
            byte buffer[MIFARE_BLOCK_SIZE];
            memcpy(buffer, item.bytes, MIFARE_BLOCK_SIZE);
            MFRC522_I2C::StatusCode status = MFRC522_I2C::STATUS_TIMEOUT;
            for (int attempt = 0; attempt < WRITE_ATTEMPTS; attempt++) {
                if (attempt > 0) {
                    // A timed-out write leaves the sector's crypto session uncertain; a re-auth
                    // without a re-select desyncs (slice-0046), so re-select and re-auth the SAME
                    // sector before retrying. authedSector is unchanged, so the sector's remaining
                    // blocks keep writing under this freshly opened session.
                    selectCard();
                    if (!authBlock(dump, item.sector, item.addr)) {
                        sectorAuthed = false;  // deterministic auth loss, not a transient write miss
                        break;
                    }
                    delay(20);
                }
                status =
                    (MFRC522_I2C::StatusCode)m_rfid->MIFARE_Write(item.addr, buffer, MIFARE_BLOCK_SIZE);
                if (status == MFRC522_I2C::STATUS_OK) break;
                LOG_D("RFID", "write: block %d attempt %d/%d NAK (status %d)",
                      item.addr, attempt + 1, WRITE_ATTEMPTS, status);
            }
            if (status == MFRC522_I2C::STATUS_OK) {
                result.blocksWritten++;
            } else if (!sectorAuthed) {
                result.failedAuth++;
                LOG_W("RFID", "write: block %d lost sector %d auth on retry",
                      item.addr, item.sector);
            } else {
                result.failedWrite++;
                LOG_W("RFID", "write: block %d NAK after %d attempts (status %d)",
                      item.addr, WRITE_ATTEMPTS, status);
            }
        }
        done++;
        if (onProgress) onProgress({done, result.totalPlanned, item.addr});
    }
    if (sectorAuthed) m_rfid->PCD_StopCrypto1();  // close the last sector's session

    if (includeUid && plan.hasManufacturer) {
        result.uidAttempted = true;
        byte buffer[MIFARE_BLOCK_SIZE];
        memcpy(buffer, plan.manufacturer.bytes, MIFARE_BLOCK_SIZE);

        // Block 0 is the manufacturer block; two card families make it rewritable by two different
        // paths, so we try both and let the outcome classify the card (slice-0048).
        //
        // Strategy 1 - Gen2/CUID direct write: an authenticated write to block 0, which only a
        // direct-write magic card accepts. A sector-0 auth failure is NOT terminal here: strategy 2
        // needs no auth, so fall through to it rather than giving up (a Gen1a whose sector-0 keys we
        // do not hold still clones its UID via the backdoor).
        selectCard();
        if (authBlock(dump, 0, 0)) {
            MFRC522_I2C::StatusCode status =
                (MFRC522_I2C::StatusCode)m_rfid->MIFARE_Write(0, buffer, MIFARE_BLOCK_SIZE);
            if (status == MFRC522_I2C::STATUS_OK) {
                result.uidWritten = true;
                result.uidResult = "UID written - Gen2/CUID direct-write magic confirmed";
            }
            m_rfid->PCD_StopCrypto1();  // close the crypto session strategy 1 opened
        }

        // Strategy 2 - Gen1a backdoor: block 0 is rewritable only after the 0x40/0x43 backdoor opens
        // it (unauthenticated, the same sequence probeMagicGen1a() ACKs for detection). Reached only
        // when the direct write did not land. A backdoor ACK followed by a write NAK is surfaced,
        // not masked - the card answered the backdoor but refused the write (fail loud).
        if (!result.uidWritten) {
            // Retry the backdoor open with a re-select between tries (OpenUidBackdoor halts the tag
            // on failure, so the next try must re-select first) — a single attempt false-negatives
            // on a real Gen1a (slice-0044 / slice-0048 on-device).
            bool backdoorOpen = false;
            for (int attempt = 0; attempt < BACKDOOR_OPEN_ATTEMPTS && !backdoorOpen; attempt++) {
                selectCard();  // clean ACTIVE state for the backdoor sequence
                backdoorOpen = m_rfid->MIFARE_OpenUidBackdoor(false);
            }
            if (!backdoorOpen) {
                result.uidResult = "UID not written - block 0 locked (ordinary card, not magic)";
            } else {
                MFRC522_I2C::StatusCode status =
                    (MFRC522_I2C::StatusCode)m_rfid->MIFARE_Write(0, buffer, MIFARE_BLOCK_SIZE);
                if (status == MFRC522_I2C::STATUS_OK) {
                    result.uidWritten = true;
                    result.uidResult = "UID written - Gen1a backdoor magic confirmed";
                } else {
                    result.uidResult =
                        "UID not written - Gen1a backdoor opened but block 0 write NAK'd";
                }
            }
        }
        done++;
        if (onProgress) onProgress({done, result.totalPlanned, 0});
    }

    // Report the data-block tally against the data-block denominator only: totalPlanned folds in the
    // +1 UID step, so pairing it with blocksWritten (data-only) read as an off-by-one (47/48) on a
    // full clone. The UID outcome is a separate concern and is carried by uidResult, not this count.
    LOG_I("RFID", "Write done: %d/%d data blocks, %d auth-fail, %d write-fail. %s",
          result.blocksWritten, static_cast<int>(plan.dataWrites.size()), result.failedAuth,
          result.failedWrite, result.uidResult.c_str());
    return result;
}

} // namespace adversary
