/**
 * @file rfid_manager.cpp
 * @brief RFIDManager implementation for WS1850S
 */

#include <Arduino.h>
#include <SPI.h>
#include <MFRC522_I2C.h>
#include "rfid_manager.h"
#include "config/pins.h"
#include "core/event_bus.h"

namespace adversary {

RFIDManager::RFIDManager()
    : m_rfid(nullptr)
    , m_detected(false)
    , m_active(false)
    , m_hasTag(false)
    , m_lastScanTime(0) {
}

bool RFIDManager::init() {
    if (m_detected) return true;

    Serial.println("[RFID] Initializing WS1850S (I2C 0x28)...");
    
    // RFID2 unit uses I2C. On Cardputer Port A Grove uses pins 2 (SDA) and 1 (SCL)
    // We must ensure Wire is initialized on these pins if we use Grove.
#if defined(TARGET_CARDPUTER)
    Wire.begin(2, 1);
#else
    Wire.begin();
#endif

    m_rfid = new MFRC522_I2C(I2C_ADDR, -1);
    
    // PCD_Init calls Reset, which ensures a clean state
    m_rfid->PCD_Init();
    delay(50); // Stabilization delay
    
    // Detection check: Read version register
    byte version = m_rfid->PCD_ReadRegister(m_rfid->VersionReg);
    
    if (version == 0x00 || version == 0xFF) {
        Serial.printf("[RFID] Module not detected! (Version: 0x%02X)\n", version);
        delete m_rfid;
        m_rfid = nullptr;
        m_detected = false;
        return false;
    }

    // Set antenna gain to MAX (48dB)
    // This is critical for reading tags that are weak or have small antennas
    // 0x07 << 4 sets the RxGain bits to the maximum value
    m_rfid->PCD_SetAntennaGain(m_rfid->RxGain_max);

    Serial.printf("[RFID] WS1850S detected. Version: 0x%02X, Gain: MAX (48dB)\n", version);
    m_detected = true;
    
    // Antena starts OFF by default to save power
    deactivate();
    
    return true;
}

bool RFIDManager::redetect() {
    // Drop the cached verdict and the reader so init() actually re-probes the bus
    // (it short-circuits while m_detected is set). This makes detection
    // bidirectional: a removed module flips m_detected back to false.
    if (m_rfid) {
        delete m_rfid;
        m_rfid = nullptr;
    }
    m_detected = false;
    m_active = false;
    return init();
}

void RFIDManager::update() {
    if (!m_detected || !m_active || !m_rfid) return;

    // Limit polling rate to 5Hz to avoid flooding I2C
    if (millis() - m_lastScanTime < 200) return;
    m_lastScanTime = millis();

    // Look for new cards
    if (!m_rfid->PICC_IsNewCardPresent()) {
        return;
    }

    // Select one of the cards
    if (!m_rfid->PICC_ReadCardSerial()) {
        return;
    }

    // Tag detected
    m_lastTag.lastSeen = millis();
    
    // ATQA is returned by PICC_RequestA/WakeupA which happened just before ReadCardSerial
    // However, the MFRC522 library doesn't always expose it. 
    // We can try to re-read it if necessary, but it's usually 2 bytes.
    // For now, we'll focus on SAK and ATS.
    m_lastTag.sak = m_rfid->uid.sak;
    
    // Convert UID to string
    char uidBuf[32];
    char* p = uidBuf;
    for (byte i = 0; i < m_rfid->uid.size; i++) {
        p += sprintf(p, "%02X", m_rfid->uid.uidByte[i]);
    }
    m_lastTag.uid = uidBuf;
    
    // Get type
    MFRC522_I2C::PICC_Type piccType = (MFRC522_I2C::PICC_Type)m_rfid->PICC_GetType(m_rfid->uid.sak);
    m_lastTag.type = String(m_rfid->PICC_GetTypeName(piccType)).c_str();

    // ISO/IEC 14443-4 Extended Details (ATS)
    m_lastTag.ats = "";
    if (m_lastTag.sak & 0x20) {
        // This is an ISO-4 tag. Attempt to get ATS via RATS command.
        byte ratsBuffer[32];
        byte ratsSize = sizeof(ratsBuffer);
        
        // RATS: 0xE0, 0x50 (0x50 = FSDI=5, which is standard)
        byte ratsCmd[] = {0xE0, 0x50};
        MFRC522_I2C::StatusCode status = (MFRC522_I2C::StatusCode)m_rfid->PCD_CommunicateWithPICC(
            MFRC522_I2C::PCD_Transceive, 0x0C, ratsCmd, 2, ratsBuffer, &ratsSize
        );
        
        if (status == MFRC522_I2C::STATUS_OK && ratsSize > 0) {
            char atsBuf[128];
            char* ptr = atsBuf;
            for (byte i = 0; i < ratsSize; i++) {
                ptr += sprintf(ptr, "%02X ", ratsBuffer[i]);
            }
            m_lastTag.ats = atsBuf;
        }
    }
    
    // Magic-card (Gen1a UID backdoor) detection. The probe HALTs and re-selects
    // the tag, so it is relatively slow and churns card state -- run it once per
    // newly-seen UID (the same edge that gates the notify), not on every 200ms
    // poll of a card that is just sitting on the reader. The verdict persists in
    // m_lastTag for the Details view until the card is cleared.
    bool isNewUid = (m_lastTag.uid != m_lastNotifiedUid);
    if (isNewUid) {
        m_lastTag.isMagic = probeMagicGen1a();
    }

    Serial.printf("[RFID] Tag Detected: %s (%s, SAK: 0x%02X, MAGIC: %s)\n",
                  m_lastTag.uid.c_str(), m_lastTag.type.c_str(), m_lastTag.sak,
                  m_lastTag.isMagic ? "YES" : "NO");
    m_hasTag = true;

    // Notify ONLY if it's a new UID detect
    if (isNewUid) {
        // Publish EventBus event for semantic notification
        EventData event(EventType::RFID_TAG_DETECTED);
        // Include partial UID in network SSID field for identification in notification router if needed
        strncpy(event.payload.network.ssid, m_lastTag.uid.c_str(), 32);
        EventBus::getInstance().publish(event);

        m_lastNotifiedUid = m_lastTag.uid;
    }

    // We do NOT Halt here anymore, to keep the tag ACTIVE for immediate auditing.
    // m_rfid->PICC_HaltA();
}

bool RFIDManager::probeMagicGen1a() {
    // Gen1a "magic" cards answer a non-standard backdoor wake (HALT, then a 7-bit
    // 0x40 and a 0x43, each ACKed with a 4-bit 0x0A) that genuine NXP silicon
    // ignores. The driver already implements that exact wire sequence, so reuse
    // it rather than re-hand-roll the raw framing. It returns true only on a full
    // backdoor ACK; Gen2/CUID "direct write" magic is protocol-indistinguishable
    // here and is intentionally out of scope (see slice-0041).
    // The backdoor wake is timing-sensitive: the first transceive after HALT often misses, so a
    // single attempt yields false negatives (observed in-field: the same Gen1a card read NO then
    // YES). A YES is high-confidence (an ordinary card cannot ACK the non-standard frames), so we
    // retry up to MAGIC_PROBE_ATTEMPTS, re-selecting the card to ACTIVE between tries, and report
    // NO only after that many clean failures (slice-0044).
    constexpr int MAGIC_PROBE_ATTEMPTS = 5;
    bool isMagic = false;
    for (int attempt = 0; attempt < MAGIC_PROBE_ATTEMPTS && !isMagic; attempt++) {
        if (attempt > 0) {
            // Re-select to ACTIVE so the retry starts from the same clean state a fresh
            // detection would; if the card will not wake, try again rather than give up.
            m_rfid->PICC_HaltA();
            byte atqa[2];
            byte atqaSize = sizeof(atqa);
            if (m_rfid->PICC_WakeupA(atqa, &atqaSize) != MFRC522_I2C::STATUS_OK ||
                !m_rfid->PICC_ReadCardSerial()) {
                continue;
            }
        }
        isMagic = m_rfid->MIFARE_OpenUidBackdoor(false);
    }
    if (isMagic) {
        Serial.println("[RFID] Magic Gen1a backdoor ACK -> block 0 / UID is rewritable");
    }

    // MIFARE_OpenUidBackdoor() begins with HALTA, so the tag is left halted
    // whether or not it was magic. Re-select it so the tag stays live for an
    // immediate audit (the invariant the "keep the tag ACTIVE" note below relies
    // on). A tag that does not come back is a real bus/tag fault, not a plain
    // "not magic" result -- surface it rather than letting a degraded reader
    // silently report every card as non-magic (fail loud). A normal card's
    // silence to the backdoor frames is the expected negative and is NOT a fault.
    m_rfid->PICC_HaltA();
    byte bufferATQA[2];
    byte bufferSize = sizeof(bufferATQA);
    if (m_rfid->PICC_WakeupA(bufferATQA, &bufferSize) != MFRC522_I2C::STATUS_OK ||
        m_rfid->PICC_Select(&m_rfid->uid, 0) != MFRC522_I2C::STATUS_OK) {
        Serial.println("[RFID] Magic probe: tag did not re-select cleanly; bus/tag state suspect");
    }
    return isMagic;
}

const RFIDTagInfo* RFIDManager::getLastTag() const {
    return m_hasTag ? &m_lastTag : nullptr;
}

void RFIDManager::clearTag() {
    if (m_hasTag && m_rfid) {
        // Halt and stop crypto when explicitly clearing, to allow re-scans
        m_rfid->PICC_HaltA();
        m_rfid->PCD_StopCrypto1();
    }
    m_hasTag = false;
    m_lastNotifiedUid = ""; // Reset to allow re-notifying if same tag returns
}

void RFIDManager::activate() {
    if (!m_detected || !m_rfid) return;
    
    Serial.println("[RFID] Activating antenna...");
    m_rfid->PCD_AntennaOn();
    m_active = true;
}

void RFIDManager::deactivate() {
    if (!m_detected || !m_rfid) return;
    
    Serial.println("[RFID] Deactivating antenna (low power)...");
    m_rfid->PCD_StopCrypto1();
    m_rfid->PICC_HaltA();
    m_rfid->PCD_AntennaOff();
    m_active = false;
}

void RFIDManager::resetForTest() {
    if (m_rfid) delete m_rfid;
    m_rfid = nullptr;
    m_detected = false;
    m_active = false;
    m_hasTag = false;
    m_lastNotifiedUid = "";
    m_lastScanTime = 0;
}

} // namespace adversary
