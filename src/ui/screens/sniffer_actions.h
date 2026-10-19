/**
 * @file sniffer_actions.h
 * @brief Pure quick-action policy for the packet sniffer screen.
 */

#pragma once

#include "sniffer_types.h"
#include "core/event_data.h"

namespace adversary {

/**
 * @brief Build the attack-target payload for a quick action on a captured packet.
 *
 * Pure and display-free: no EventBus, no logging, no screen state, so it is unit-testable
 * off-device (SnifferScreen is not host-constructible). Returns false for actions that launch
 * no attack (CANCEL or any unmapped action), leaving @p out untouched; otherwise returns true
 * and fills @p out with the target screen plus the packet's source MAC, SSID and channel.
 *
 * The caller must pass a snapshot of the selected packet taken when the action menu opened, not
 * a live index into the capture ring: the ring is overwritten in place as new frames arrive, so
 * an index would resolve to a different packet by the time the action runs.
 */
bool makeSnifferAttackTarget(const PacketSummary& packet, PacketAction action,
                             AttackTargetEventData& out);

/**
 * @brief Map an oldest-first display row to its physical slot in the in-place capture ring.
 *
 * The packet log is a fixed-capacity ring overwritten in place: while it fills, @p head stays
 * 0 and physical order equals capture order; once it wraps, @p head is the oldest slot and the
 * linear display index no longer matches capture order. This resolves display row @p
 * displayIndex (0 = oldest shown, size-1 = newest) to the physical index into the ring.
 *
 * Pure and display-free so the wrap arithmetic is unit-testable off-device. Precondition
 * enforced by the callers: @p size > 0, @p head < @p size, and 0 <= @p displayIndex < @p size.
 */
size_t snifferLogPhysicalIndex(size_t head, size_t size, int displayIndex);

} // namespace adversary
