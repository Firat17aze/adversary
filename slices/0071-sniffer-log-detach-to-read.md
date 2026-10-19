---
id: '0071'
title: "Sniffer packet log freezes for reading when scrolled off the live tail"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

Make the live packet log readable without reaching for Pause. Today, in a busy RF
environment the log is an unreadable blur: once the ring is full every captured frame evicts
the oldest entry and slides the whole display up one row, and only manual Pause stops it. The
operator surfaced this while confirming slice-0070; it is pre-existing, not caused by that fix.

Freeze the log for reading the moment the operator scrolls up off the newest row, and resume
the live stream when they scroll back to it. Raise the ring 25 -> 50 for more scrollback to
page through while frozen.

## Definition of Done

- **Given** the ring is full and capturing at a high frame rate
- **When** the operator scrolls up off the newest row
- **Then** the rows stay completely fixed on screen and stay readable, indefinitely, however
  fast frames keep arriving

- **Given** the operator has scrolled up (the log is frozen)
- **When** frames keep arriving
- **Then** capture is not lost: stats keep counting, deauth-flood detection keeps running, and
  SD capture (when enabled) keeps writing every frame

- **Given** the operator has scrolled up (the log is frozen)
- **When** they scroll back down to the newest row
- **Then** the log resumes following the live tail

- **Given** the log is frozen by scrolling up and not manually paused
- **When** it renders
- **Then** a `[REVIEW - capturing]` indicator shows, distinct from the manual `[PAUSED]` banner

## Design

The earlier attempt (slice-0071 draft) tried to keep the ring sliding but compensate the
scroll window (`ListScrollModel::dropFront`) so the reader's rows stayed put. Measured against
the real problem it cannot win: a full 50-slot ring evicts the ~42 off-screen rows in well
under a second at beacon rates, so the frozen view held for an imperceptible moment and then
slid again, and no affordable buffer changes that. That approach was reverted (model and its
tests restored unchanged).

The key observation is that manual Pause already only freezes the log *display*:
`onPacketReceived` updates stats, runs flood detection and queues the SD write outside the
pause gate, so freezing the log loses nothing real. The log is also only ever a rolling
last-50 window, so frames arriving while the operator reads would have evicted the rows being
read anyway.

So the fix stops the slide at its source: gate the log append on the view following the live
tail, not only on manual pause. `onPacketReceived` appends only when `!m_logPaused &&
isLogFollowingTail()`, where `isLogFollowingTail()` is true while the selection sits on the
newest row (an empty log follows by definition). Scrolling up off the tail makes it false, so
the ring freezes instantly and stays frozen at any frame rate; scrolling back to the newest
row resumes appends. Because appends now happen only while following, `addPacketToLog` simply
pins the selection to the new tail. A `[REVIEW - capturing]` cue renders while frozen-by-scroll
and unpaused, to reassure the operator capture is still live and show that returning to the
newest row resumes the stream. Manual Pause is kept: it still freezes in place while on the
tail, and composes with the scroll freeze since both just stop appends.

`PACKET_LOG_SIZE` goes 25 -> 50, now justified purely as scrollback depth (measured
`sizeof(PacketSummary) == 40` B, so 2,000 B reserved once at construction, up 1,000 on the
~205 KB no-PSRAM budget); the stale "Save ~1.5KB" comment is corrected to the measured figure.

**What existing code was considered.** Reused the existing `m_logPaused` display-freeze
semantics rather than adding a parallel pause concept, and the existing `ListScrollModel`
selection as the single source of "are we at the tail", so the gate is one predicate over state
the screen already owns. Considered and rejected extending `ListScrollModel` (the reverted
`dropFront`): with the append-gate approach there are no evictions while detached, so there is
nothing to compensate and the shared model stays untouched for its other 8 consumers. The
slice-0070 physical-index mapping is unchanged and composes: while frozen the ring head does not
move, so the mapping is stable.

## Verification

- `pio test -e native` green (844/844). The behaviour is screen-level in `SnifferScreen`, which
  is not host-constructible (the same constraint recorded for the pure half in slice-0070), so
  it carries no new host test; `ListScrollModel` is unchanged and its suite is restored to its
  prior state. Covered on device by the DoD below.
- `pio run -e cardputer` SUCCESS, Flash 74.6%, 2,492,075 bytes.
- On-device DoD: with a busy capture, scroll up and confirm the rows hold still and stay
  readable while the stats keep climbing (capture still live) and `[REVIEW - capturing]` shows;
  scroll back to the newest row and confirm the live stream resumes; confirm manual Pause still
  freezes at the tail.

## As built

Shipped as the freeze-on-scroll design above, after the `dropFront` sliding-window attempt was
proven on device to hold for only an imperceptible moment at beacon rates and was reverted
(`ListScrollModel` and its test restored to zero diff vs HEAD). `onPacketReceived` now appends
to the log only while `!m_logPaused && isLogFollowingTail()`; `isLogFollowingTail()` is true while
the selection sits on the newest row (empty log follows by definition). `addPacketToLog` pins the
selection to the new tail. A `[REVIEW - capturing]` banner renders while frozen-by-scroll and
unpaused, beside the retained manual `[PAUSED]` banner. `PACKET_LOG_SIZE` is 50, re-justified as
scrollback depth, and the stale byte-estimate comment is corrected to the measured 40 B/entry.

Measured: `pio test -e native` 844/844 (`ListScrollModel` unchanged, so no suite delta).
`pio run -e cardputer` SUCCESS, Flash 74.6%, 2,492,075 bytes (+96 vs be519dd). The behaviour is
screen-level and device-only, so it carries no host test, as noted in Verification. On device the
operator confirmed all four DoD scenarios: scrolling up freezes the rows readably at full frame
rate with `[REVIEW - capturing]` shown, the stats keep climbing while frozen (capture still live),
returning to the newest row resumes the stream, and manual Pause still freezes at the tail.
