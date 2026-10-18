/**
 * @file test_sniffer_actions.cpp
 * @brief Regression tests for the sniffer quick-action target policy.
 *
 * The packet log's action menu is built from a snapshot of the selected packet and must act on
 * THAT packet, not a live index into the capture ring (the ring overwrites entries in place as
 * frames arrive, and auto-scroll can move the selection while the menu is open). SnifferScreen is
 * not host-constructible, so the snapshot decoupling itself is verified by inspection; this suite
 * locks down the pure half extracted to make it testable: each action routes to the right attack
 * screen, CANCEL/unmapped actions launch nothing, and the emitted target carries exactly the
 * given packet's source MAC, SSID and channel.
 */

#include <unity.h>
#include "ui/screens/sniffer_actions.h"
#include "ui/screens/screen_interface.h"  // ScreenId
#include <cstring>

using namespace adversary;

void setUp(void) {}
void tearDown(void) {}

static PacketSummary makePacket(PacketFrameType type, const uint8_t mac[6],
                                const char* ssid, uint8_t channel) {
    PacketSummary p;
    p.frameType = type;
    memcpy(p.srcMac, mac, 6);
    strncpy(p.ssid, ssid, sizeof(p.ssid) - 1);
    p.ssid[sizeof(p.ssid) - 1] = '\0';
    p.channel = channel;
    return p;
}

// Each attack action routes to its own attack screen.
void test_handshake_routes_to_handshake_screen() {
    uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    PacketSummary pkt = makePacket(PacketFrameType::EAPOL, mac, "Net", 6);
    AttackTargetEventData out;
    TEST_ASSERT_TRUE(makeSnifferAttackTarget(pkt, PacketAction::HANDSHAKE_CAPTURE, out));
    TEST_ASSERT_EQUAL_INT16((int16_t)ScreenId::HANDSHAKE, out.targetScreen);
}

void test_deauth_routes_to_deauth_screen() {
    uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    PacketSummary pkt = makePacket(PacketFrameType::BEACON, mac, "Net", 6);
    AttackTargetEventData out;
    TEST_ASSERT_TRUE(makeSnifferAttackTarget(pkt, PacketAction::DEAUTH_ATTACK, out));
    TEST_ASSERT_EQUAL_INT16((int16_t)ScreenId::DEAUTH, out.targetScreen);
}

void test_evil_twin_routes_to_evil_twin_screen() {
    uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    PacketSummary pkt = makePacket(PacketFrameType::PROBE_RESP, mac, "Net", 6);
    AttackTargetEventData out;
    TEST_ASSERT_TRUE(makeSnifferAttackTarget(pkt, PacketAction::EVIL_TWIN, out));
    TEST_ASSERT_EQUAL_INT16((int16_t)ScreenId::EVIL_TWIN, out.targetScreen);
}

void test_karma_routes_to_karma_screen() {
    uint8_t mac[6] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    PacketSummary pkt = makePacket(PacketFrameType::PROBE_REQ, mac, "Net", 6);
    AttackTargetEventData out;
    TEST_ASSERT_TRUE(makeSnifferAttackTarget(pkt, PacketAction::KARMA_ATTACK, out));
    TEST_ASSERT_EQUAL_INT16((int16_t)ScreenId::KARMA, out.targetScreen);
}

// CANCEL launches no attack and leaves the caller's payload untouched.
void test_cancel_returns_false_and_leaves_out_untouched() {
    uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
    PacketSummary pkt = makePacket(PacketFrameType::PROBE_REQ, mac, "X", 1);
    AttackTargetEventData out;
    out.targetScreen = 0x7B;
    out.channel = 99;
    TEST_ASSERT_FALSE(makeSnifferAttackTarget(pkt, PacketAction::CANCEL, out));
    TEST_ASSERT_EQUAL_INT16(0x7B, out.targetScreen);  // untouched
    TEST_ASSERT_EQUAL_UINT8(99, out.channel);         // untouched
}

// The emitted target carries THIS packet's identity: the fidelity the snapshot fix protects.
void test_target_carries_selected_packet_fields() {
    uint8_t mac[6] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01};
    PacketSummary pkt = makePacket(PacketFrameType::PROBE_RESP, mac, "TargetNet", 11);
    AttackTargetEventData out;
    TEST_ASSERT_TRUE(makeSnifferAttackTarget(pkt, PacketAction::EVIL_TWIN, out));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(mac, out.bssid, 6);
    TEST_ASSERT_EQUAL_STRING("TargetNet", out.ssid);
    TEST_ASSERT_EQUAL_UINT8(11, out.channel);
}

// Two different packets yield two independent targets (no shared or stale state leaks through).
void test_distinct_packets_yield_distinct_targets() {
    uint8_t macA[6] = {0xAA, 0, 0, 0, 0, 0xA1};
    uint8_t macB[6] = {0xBB, 0, 0, 0, 0, 0xB2};
    PacketSummary a = makePacket(PacketFrameType::BEACON, macA, "APA", 1);
    PacketSummary b = makePacket(PacketFrameType::BEACON, macB, "APB", 6);
    AttackTargetEventData oa, ob;
    TEST_ASSERT_TRUE(makeSnifferAttackTarget(a, PacketAction::DEAUTH_ATTACK, oa));
    TEST_ASSERT_TRUE(makeSnifferAttackTarget(b, PacketAction::DEAUTH_ATTACK, ob));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(macA, oa.bssid, 6);
    TEST_ASSERT_EQUAL_STRING("APA", oa.ssid);
    TEST_ASSERT_EQUAL_UINT8(1, oa.channel);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(macB, ob.bssid, 6);
    TEST_ASSERT_EQUAL_STRING("APB", ob.ssid);
    TEST_ASSERT_EQUAL_UINT8(6, ob.channel);
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_handshake_routes_to_handshake_screen);
    RUN_TEST(test_deauth_routes_to_deauth_screen);
    RUN_TEST(test_evil_twin_routes_to_evil_twin_screen);
    RUN_TEST(test_karma_routes_to_karma_screen);
    RUN_TEST(test_cancel_returns_false_and_leaves_out_untouched);
    RUN_TEST(test_target_carries_selected_packet_fields);
    RUN_TEST(test_distinct_packets_yield_distinct_targets);
    return UNITY_END();
}
