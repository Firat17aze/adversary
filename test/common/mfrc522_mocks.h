#pragma once

/**
 * @file mfrc522_mocks.h
 * @brief Standards-compliant mock for MFRC522_I2C to match real library
 */

#ifndef ESP32
#pragma message("USING REALISTIC MFRC522 MOCK")

#include <stdint.h>
#include <cstring>
#include <vector>

typedef uint8_t byte;

// Settable VERSION byte so tests can simulate a present (0x92) or an
// absent (0x00 / 0xFF) reader — including across a redetect()'s fresh
// `new MFRC522_I2C`, which a per-instance value could not survive (slice-0018).
inline byte g_mockMfrcVersion = 0x92;

// Settable Gen1a-backdoor verdict so tests can simulate a magic card (true) or
// an ordinary one (false, the default so existing RFID tests are untouched).
// Mirrors the g_mockMfrcVersion seam: a free global survives the fresh
// `new MFRC522_I2C` a redetect()/re-open creates, which a per-instance field
// could not.
inline bool g_mockMagicGen1a = false;

// Probe-retry seam (slice-0044): if >0, MIFARE_OpenUidBackdoor returns false until its
// g_mockMagicAckOnAttempt-th call, then true — modelling a flaky Gen1a backdoor that only ACKs
// after a retry. 0 (default) keeps the plain g_mockMagicGen1a behaviour so other tests are
// untouched. g_mockBackdoorCalls counts calls across the fresh `new MFRC522_I2C` a probe creates.
inline int g_mockMagicAckOnAttempt = 0;
inline int g_mockBackdoorCalls = 0;

// --- RFID Write test seams (slice-0043) ---
// A recording MIFARE_Write so test_rfid_writer can assert which blocks were written with which
// bytes. Cleared by tests in setUp(). Free globals for the same reason as the verdict above.
struct MockMifareWrite {
    uint8_t addr;
    uint8_t bytes[16];
};
inline std::vector<MockMifareWrite> g_mockWrites;

// When true, MIFARE_Write to block 0 returns a NAK — models an ordinary card's locked
// manufacturer block, so the UID-write fail-loud path is exercisable off-target.
inline bool g_mockBlock0Nak = false;

// When true, PCD_Authenticate rejects every key — models a sector whose keys are unknown to the
// target, so the writer's failedAuth path is reachable despite the factory-FF fallback.
inline bool g_mockAuthFails = false;

// When true, MIFARE_Read returns an error even after a successful auth — models a block that
// authenticates but will not read back, so the audit's read-fail diagnostic is distinguishable
// from its auth-fail diagnostic (slice-0045).
inline bool g_mockReadFails = false;

// Re-auth desync model (slice-0046): a real WS1850S/clone NAKs a second PCD_Authenticate issued
// without an intervening card re-select, even after StopCrypto1 — the root cause of blocks 2-4 of
// every sector reading '??'. When on, a successful auth "consumes" the current selection and the
// next auth fails (STATUS_ERROR) until a re-select (WakeupA/RequestA/ReadCardSerial/Select)
// restores it. Off by default so the per-block callers' existing tests are unaffected.
inline bool g_mockReauthNeedsReselect = false;
inline bool g_mockSelectedSinceAuth = true;

class MFRC522_I2C {
public:
    enum PCD_Register : byte {
        VersionReg = 0x37
    };

    enum PCD_Command : byte {
        PCD_Idle = 0x00,
        PCD_Mem = 0x01,
        PCD_GenerateRandomID = 0x02,
        PCD_CalcCRC = 0x03,
        PCD_Transceive = 0x0C,
        PCD_MFAuthent = 0x0E,
        PCD_SoftReset = 0x0F
    };

    enum PICC_Command : byte {
        PICC_CMD_REQA = 0x26,
        PICC_CMD_WUPA = 0x52,
        PICC_CMD_MF_AUTH_KEY_A = 0x60,
        PICC_CMD_MF_AUTH_KEY_B = 0x61,
        PICC_CMD_HLTA = 0x50
    };

    enum StatusCode : byte {
        STATUS_OK = 1,
        STATUS_ERROR = 2,
        STATUS_COLLISION = 3,
        STATUS_TIMEOUT = 4,
        STATUS_NO_ROOM = 5,
        STATUS_INTERNAL_ERROR = 6,
        STATUS_INVALID = 7,
        STATUS_CRC_WRONG = 8,
        STATUS_MIFARE_NACK = 9
    };

    typedef struct {
        byte size;
        byte uidByte[10];
        byte sak;
    } Uid;

    typedef struct {
        byte keyByte[6];
    } MIFARE_Key;

    Uid uid;

    MFRC522_I2C(byte addr, int8_t resetPin) {
        uid.size = 4;
        uid.sak = 0x08; // MIFARE Classic 1K
        memset(uid.uidByte, 0xDE, 4);
    }
    
    void PCD_Init() {}
    byte PCD_ReadRegister(byte reg) { return g_mockMfrcVersion; } // default 0x92 (Version 2.0)
    void PCD_SetAntennaGain(byte gain) {}
    void PCD_AntennaOn() {}
    void PCD_AntennaOff() {}
    bool PICC_IsNewCardPresent() { return true; }
    bool PICC_ReadCardSerial() { g_mockSelectedSinceAuth = true; return true; }
    void PICC_HaltA() {}
    void PCD_StopCrypto1() {}  // intentionally does NOT re-select: StopCrypto1 alone cannot clear
                               // the desync the real card exhibits (slice-0046).

    byte PICC_WakeupA(byte* bufferATQA, byte* bufferSize) { g_mockSelectedSinceAuth = true; return STATUS_OK; }
    byte PICC_RequestA(byte* bufferATQA, byte* bufferSize) { g_mockSelectedSinceAuth = true; return STATUS_OK; }
    byte PICC_Select(Uid* uid, byte validBits = 0) { g_mockSelectedSinceAuth = true; return STATUS_OK; }

    // Gen1a backdoor probe: a real magic card ACKs the 0x40/0x43 sequence; an
    // ordinary card does not. The real driver collapses "ordinary card" and
    // "bus fault" into a single false, which is faithful to model here -- the
    // caller's fail-loud handling lives in the post-probe re-select, not here.
    bool MIFARE_OpenUidBackdoor(bool /*logErrors*/) {
        g_mockBackdoorCalls++;
        if (g_mockMagicAckOnAttempt > 0) return g_mockBackdoorCalls >= g_mockMagicAckOnAttempt;
        return g_mockMagicGen1a;
    }

    byte PCD_Authenticate(byte command, byte blockAddr, MIFARE_Key* key, Uid* uid) {
        if (g_mockAuthFails) return STATUS_ERROR;
        // A re-auth without an intervening re-select NAKs on the real reader (slice-0046).
        if (g_mockReauthNeedsReselect && !g_mockSelectedSinceAuth) return STATUS_ERROR;
        bool keyOk = (key->keyByte[0] == 0xFF && key->keyByte[1] == 0xFF) ||
                     (key->keyByte[0] == 0xA0 && key->keyByte[1] == 0xA1);
        if (!keyOk) return STATUS_ERROR;
        g_mockSelectedSinceAuth = false;  // a successful auth consumes the selection
        return STATUS_OK;
    }

    byte MIFARE_Write(byte blockAddr, byte* buffer, byte bufferSize) {
        MockMifareWrite record;
        record.addr = blockAddr;
        memset(record.bytes, 0, sizeof(record.bytes));
        if (buffer) {
            memcpy(record.bytes, buffer, bufferSize < 16 ? bufferSize : 16);
        }
        g_mockWrites.push_back(record);
        if (blockAddr == 0 && g_mockBlock0Nak) return STATUS_MIFARE_NACK;
        return STATUS_OK;
    }

    enum PICC_Type : byte {
        PICC_TYPE_UNKNOWN = 0,
        PICC_TYPE_MIFARE_1K = 4
    };

    const char* PICC_GetTypeName(byte type) { return "Mock Tag"; }
    byte PICC_GetType(byte sak) { return PICC_TYPE_MIFARE_1K; }

    static const byte RxGain_max = 0x07 << 4;

    byte PCD_CommunicateWithPICC(byte command, byte waitIRq, byte* sendData, byte sendLen, byte* backData = nullptr, byte* backLen = nullptr) {
        return STATUS_OK;
    }

    byte MIFARE_Read(byte blockAddr, byte* buffer, byte* bufferSize) {
        if (g_mockReadFails) return STATUS_ERROR;
        if (buffer && bufferSize && *bufferSize >= 18) {
            memset(buffer, 0, 18);
        }
        return STATUS_OK;
    }
};

#endif
