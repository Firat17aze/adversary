---
id: '0070'
title: "Sniffer packet log renders in capture order after the ring wraps"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

Fix the sniffer packet-log ordering bug deferred from the wave-2c migration (slice-0068):
once the 25-slot capture ring wraps, the log shows frames in physical array order, not
capture order, so the newest packets appear interleaved mid-list instead of at the tail.
This closes the `[bug]` ledger item for `m_packetLog` physical-order rendering.

`m_packetLog` is a fixed-capacity (`PACKET_LOG_SIZE = 25`) in-place ring: while it is
filling, `m_packetLogHead` stays `0` and physical order equals capture order, so the bug is
invisible; once full, each new frame overwrites the slot at `m_packetLogHead` and advances
it, so `m_packetLogHead` is the oldest slot and `(m_packetLogHead - 1)` the newest. The
render callback and the Enter-to-act snapshot index the vector linearly `0..size-1`, which
after the first wrap no longer corresponds to capture order.

## Definition of Done

- **Given** fewer than 25 packets have been captured (the ring has not wrapped)
- **When** the operator views the packet log
- **Then** rows read oldest-at-top to newest-at-bottom, unchanged from today

- **Given** more than 25 packets have been captured (the ring has wrapped at least once)
- **When** the operator views the packet log
- **Then** the oldest retained frame is the top row, the newest is the bottom row, and the
  rows in between are in capture order with no mid-list discontinuity

- **Given** the ring has wrapped and the operator selects a row and opens its action menu
- **When** an attack action is chosen
- **Then** the attack targets the packet shown on that row, not a different physical slot

## Design

The display is oldest-first (newest at the bottom), kept deliberately: the auto-follow
(`setSelectedIndex(newCount - 1)` pins the tail) and the `[sel/total]` counter already treat
the last row as the newest, terminal-log style. The single missing piece is mapping a
display row to its physical ring slot; both auto-follow and the counter are already correct
in display terms and need no change once the mapping lands. Newest-first was rejected: it
would force inverting auto-follow and the counter for no functional gain.

The mapping is `physical(d) = (head + d) % size`, correct in both phases because `head` is
`0` until the ring wraps, where it reduces to the identity the screen uses today. It is
extracted as a pure free function `snifferLogPhysicalIndex(head, size, displayIndex)` in
`ui/screens/sniffer_actions.{h,cpp}` (the existing display-free, native-compiled companion
to the screen, home of `makeSnifferAttackTarget`) so the wrap arithmetic, the exact thing
that was gotten wrong, carries an off-device regression test. The screen calls it at the two
read sites that resolve a display row to a `PacketSummary`: the `drawPacketLogEntry` render
callback and the Enter-to-act snapshot.

**What existing code was considered.** Searched the sniffer screen for every site that
indexes `m_packetLog` by a list position: the render callback and the Enter snapshot are the
only two (the counter and auto-follow use list/display indices, not physical slots, so they
are already correct). Considered folding the mapping into `ListScrollModel`, and rejected it:
the model is a display-order-agnostic selection window shared by many screens and knows
nothing of ring buffers, so teaching it ring semantics would be the wrong abstraction for one
consumer. Considered replacing the hand-rolled vector+head ring with a reusable ring type,
and rejected it as speculative generality: there is one consumer and the bug is the index
mapping, not the storage. The mapping lives beside `makeSnifferAttackTarget` because that is
already the screen's pure, tested half.

## Verification

- `test/test_sniffer_actions/` gains tests for `snifferLogPhysicalIndex`: identity before the
  ring wraps; after a wrap (`head = 5`, `size = 25`) the oldest display row maps to slot 5 and
  the newest to slot 4; a just-wrapped ring (`head = 1`) maps newest to the slot just
  overwritten; and across a full ring the mapping is a permutation of `0..size-1` (every slot
  shown exactly once). Each fails against the identity mapping the screen used before the fix.
- `pio test -e native` green.
- `pio run -e cardputer` links (the screen wiring is device-only).
- On-device DoD: capture >25 frames and confirm the newest sits at the tail with no mid-list
  jump, and that acting on a row targets the packet it shows.

## As built

Shipped as designed. `snifferLogPhysicalIndex(head, size, displayIndex) = (head + d) % size`
was added to `ui/screens/sniffer_actions.{h,cpp}` and called at the two read sites in
`sniffer_screen.cpp`: the `drawPacketLogEntry` render callback and the Enter-to-act snapshot.
Display order stays oldest-first; auto-follow and the `[sel/total]` counter were already
correct in display terms and were left unchanged.

Four regression tests were added to `test/test_sniffer_actions`: identity before the wrap,
oldest-to-top / newest-to-bottom after a wrap (`head = 5`), newest mapped to the just-overwritten
slot for a single wrap (`head = 1`), and a full-ring bijection (every slot shown exactly once).
Each fails against the identity indexing used before the fix.

Measured: `pio test -e native` 844/844 (was 840, +4). `pio run -e cardputer` SUCCESS, Flash
74.6%, 2,491,979 bytes (-136 vs dd97c75: the extracted call let the compiler drop the three
inline modulo copies). On device the operator confirmed DoD scenarios 1-3: before the wrap the
order is unchanged, after the wrap the newest frame stays pinned at the tail with no mid-list
discontinuity, and acting on a row targets the packet shown on that row. Closes the `[bug]`
ledger item for physical-order packet-log rendering.

The operator also surfaced that the live log is unreadable without pausing (the full ring slides
the whole view up one row per captured frame, and pause is the only thing that stops ingestion).
That is a pre-existing behaviour, not introduced here, and is tracked as its own follow-up
(detach-to-read plus a larger buffer).
