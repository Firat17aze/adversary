/**
 * @file rfid_writer.cpp
 * @brief RfidWriter implementation.
 */

#include "rfid_writer.h"

#include <cstring>

namespace adversary {

namespace {
constexpr uint8_t FACTORY_KEY[MIFARE_KEY_SIZE] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
}

RfidWriter::RfidWriter(MFRC522_I2C* rfid) : m_rfid(rfid) {}

bool RfidWriter::selectCard() {
    if (!m_rfid) return false;
    for (int retry = 0; retry < 5; retry++) {
        m_rfid->PCD_StopCrypto1();
        m_rfid->PICC_HaltA();
        delay(20);

        byte bufferATQA[2];
        byte bufferSize = sizeof(bufferATQA);
        MFRC522_I2C::StatusCode status =
            (MFRC522_I2C::StatusCode)m_rfid->PICC_WakeupA(bufferATQA, &bufferSize);
        if (status != MFRC522_I2C::STATUS_OK) {
            status = (MFRC522_I2C::StatusCode)m_rfid->PICC_RequestA(bufferATQA, &bufferSize);
        }
        if ((status == MFRC522_I2C::STATUS_OK || status == MFRC522_I2C::STATUS_COLLISION) &&
            m_rfid->PICC_ReadCardSerial()) {
            return true;
        }
        delay(30 * (retry + 1));
    }
    return false;
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
        Serial.println("[RFID] Write Error: card not responding to WUPA");
        return result;
    }
    result.cardSelected = true;

    int done = 0;
    for (const WritePlanItem& item : plan.dataWrites) {
        if (!authBlock(dump, item.sector, item.addr)) {
            result.failedAuth++;
            Serial.printf("[RFID] Write: sector %d auth failed, block %d skipped\n",
                          item.sector, item.addr);
        } else {
            byte buffer[MIFARE_BLOCK_SIZE];
            memcpy(buffer, item.bytes, MIFARE_BLOCK_SIZE);
            MFRC522_I2C::StatusCode status =
                (MFRC522_I2C::StatusCode)m_rfid->MIFARE_Write(item.addr, buffer, MIFARE_BLOCK_SIZE);
            if (status == MFRC522_I2C::STATUS_OK) {
                result.blocksWritten++;
            } else {
                result.failedWrite++;
                Serial.printf("[RFID] Write: block %d NAK (status %d)\n", item.addr, status);
            }
            m_rfid->PCD_StopCrypto1();
        }
        done++;
        if (onProgress) onProgress({done, result.totalPlanned, item.addr});
    }

    if (includeUid && plan.hasManufacturer) {
        result.uidAttempted = true;
        // Block 0 lives in sector 0; re-select after the data writes, then attempt the write.
        selectCard();
        if (!authBlock(dump, 0, 0)) {
            result.uidResult = "UID not written - sector 0 auth failed";
        } else {
            byte buffer[MIFARE_BLOCK_SIZE];
            memcpy(buffer, plan.manufacturer.bytes, MIFARE_BLOCK_SIZE);
            MFRC522_I2C::StatusCode status =
                (MFRC522_I2C::StatusCode)m_rfid->MIFARE_Write(0, buffer, MIFARE_BLOCK_SIZE);
            if (status == MFRC522_I2C::STATUS_OK) {
                result.uidWritten = true;
                result.uidResult = "UID written - Gen2/CUID direct-write magic confirmed";
            } else {
                result.uidResult = "UID not written - block 0 locked (ordinary / not Gen2-CUID)";
            }
            m_rfid->PCD_StopCrypto1();
        }
        done++;
        if (onProgress) onProgress({done, result.totalPlanned, 0});
    }

    Serial.printf("[RFID] Write done: %d/%d written, %d auth-fail, %d write-fail. %s\n",
                  result.blocksWritten, result.totalPlanned, result.failedAuth,
                  result.failedWrite, result.uidResult.c_str());
    return result;
}

} // namespace adversary
