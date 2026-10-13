#include "rfid_reselect.h"

#include <Arduino.h>

namespace adversary {
namespace {

// A single wake is flaky (cold field; the Gen1a backdoor wake false-negatives, slice-0044), so
// retry before declaring the tag absent.
constexpr int RESELECT_ATTEMPTS = 5;
// Let the field settle after HALT before WUPA so the tag has actually dropped to the halted state.
constexpr uint32_t RESELECT_HALT_SETTLE_MS = 20;
// Back off a little longer after each failed attempt.
constexpr uint32_t RESELECT_RETRY_BASE_MS = 30;

} // namespace

bool reselectCard(MFRC522_I2C* rfid) {
    if (!rfid) return false;
    for (int attempt = 0; attempt < RESELECT_ATTEMPTS; attempt++) {
        // HALT before WUPA: an ACTIVE tag (left so by the previous sector's auth) ignores WUPA, so
        // wake it only after forcing it to the halted state (slice-0046).
        rfid->PCD_StopCrypto1();
        rfid->PICC_HaltA();
        delay(RESELECT_HALT_SETTLE_MS);

        byte bufferATQA[2];
        byte bufferSize = sizeof(bufferATQA);
        MFRC522_I2C::StatusCode status =
            (MFRC522_I2C::StatusCode)rfid->PICC_WakeupA(bufferATQA, &bufferSize);
        if (status != MFRC522_I2C::STATUS_OK) {
            // Fallback for a tag that was in IDLE rather than HALT (never properly halted).
            status = (MFRC522_I2C::StatusCode)rfid->PICC_RequestA(bufferATQA, &bufferSize);
        }
        if ((status == MFRC522_I2C::STATUS_OK || status == MFRC522_I2C::STATUS_COLLISION) &&
            rfid->PICC_ReadCardSerial()) {
            return true;
        }
        delay(RESELECT_RETRY_BASE_MS * (attempt + 1));
    }
    return false;
}

} // namespace adversary
