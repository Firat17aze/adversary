/**
 * @file sniffer_types.h
 * @brief Display-free data types shared by the packet sniffer screen and its
 *        pure quick-action policy (sniffer_actions).
 *
 * Split out of sniffer_screen.h so the quick-action logic can be unit-tested off-device:
 * sniffer_screen.h drags in esp_wifi/M5GFX through packet_sniffer.h and is excluded from the
 * native build, whereas these are plain PODs that compile anywhere.
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace adversary {

/**
 * @brief Frame type for packet log display
 */
enum class PacketFrameType : uint8_t {
    BEACON = 0,
    PROBE_REQ = 1,
    PROBE_RESP = 2,
    DATA = 3,
    EAPOL = 4,
    DEAUTH = 5
};

/**
 * @brief EAPOL message type for handshake tracking
 */
enum class EapolMsgType : uint8_t {
    UNKNOWN = 0,
    M1 = 1,
    M2 = 2,
    M3 = 3,
    M4 = 4
};

/**
 * @brief Lightweight packet summary for log display
 */
struct PacketSummary {
    uint32_t timestamp;
    PacketFrameType frameType;
    EapolMsgType eapolMsg;
    int8_t rssi;
    uint8_t srcMac[6];
    uint8_t dstMac[6];
    char ssid[17];
    uint8_t channel;
    uint8_t flags;

    PacketSummary()
        : timestamp(0)
        , frameType(PacketFrameType::BEACON)
        , eapolMsg(EapolMsgType::UNKNOWN)
        , rssi(-100)
        , channel(0)
        , flags(0)
    {
        memset(srcMac, 0, sizeof(srcMac));
        memset(dstMac, 0, sizeof(dstMac));
        memset(ssid, 0, sizeof(ssid));
    }
};

/**
 * @brief Quick action types available from the packet log
 */
enum class PacketAction : uint8_t {
    HANDSHAKE_CAPTURE,
    DEAUTH_ATTACK,
    EVIL_TWIN,
    KARMA_ATTACK,
    CANCEL
};

} // namespace adversary
