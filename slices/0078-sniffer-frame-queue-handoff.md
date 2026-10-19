---
id: '0078'
title: "Fix the sniffer heap-corruption data race by handing frames to the main loop over a FreeRTOS queue"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

Close the ledger bug "Sniffer capture path corrupts the heap via an unsynchronized cross-task data
race -> reboot." `PacketSniffer::promiscuousCallback` runs in the WiFi driver task and invokes the
screen callback `SnifferScreen::onPacketReceived` (`packet_sniffer.cpp:414`), which mutates three
containers shared with the main loop under **no** synchronization: it `push`es the `std::queue`
`m_packetQueue` that `processPacketQueue` concurrently `pop`s, it `push_back`s / overwrites the
`std::vector` `m_packetLog` that `render` concurrently iterates, and it increments the `m_stats`
counters `drawStats` concurrently reads. `std::queue` and `std::vector` are not thread-safe and the
ESP32-S3 is dual-core, so the WiFi task and the main loop touch the same allocator metadata at the
same instant and smash it (`CORRUPT HEAP: Expected 0xabba1234 got <ptr>`), detected later at an
unrelated free: the reported backtrace frees the `m_packetQueue` deque in `~SnifferScreen` during
exit-to-menu. Reproduced 2026-10-19 while testing slice-0077: sniff heavy traffic -> packet log ->
launch Deauth -> ESC back -> ENTER reboots. The attack round-trip is not the cause, it only widens
the race window; the race is live on every captured frame.

This is a release blocker for 0.4.0-alpha specifically because the README lists Packet Sniffer under
**"Stable - field-tested"** (`README.md:154`). A provable heap-corruption reboot in a feature we
advertise as stable has to be fixed, not documented as a known issue, before the tag.

The fix is the one CLAUDE.md's concurrency rules name directly ("Callbacks from WiFi are in ISR
context - defer work to task", "Use FreeRTOS queues for task communication"): the WiFi-task callback
stops touching every shared container and instead copies the frame into a plain-old-data struct and
hands it to the main loop over a FreeRTOS queue; the main loop drains the queue and does **all**
stats / log / pcap work, so every shared-container mutation lives on one task and the race is gone
by construction (single reader, single writer, no lock needed on the std containers because only the
main loop ever touches them).

The author already made the sibling EventBus path safe from this same callback
(`EventBus::queue`, `packet_sniffer.cpp:406`); this slice brings the direct screen callback up to the
same discipline. The secondary footer-label desync and the undocumented channel keys noted alongside
the bug are **out of scope** here (separate concerns, tracked on in the ledger) so this stays one
logical change.

## Definition of Done

- **Given** the reported repro (sniff a busy channel, open the packet log, select a packet, launch
  Deauth, press ESC back to the sniffer, press ENTER)
- **When** it is performed on the Cardputer
- **Then** the device does not reboot and prints no `CORRUPT HEAP`; the sniffer exits to the menu
  cleanly, and the same repro run ten times over does not corrupt the heap

- **Given** the sniffer is capturing under heavy traffic
- **When** frames arrive in the WiFi-driver-task callback
- **Then** the callback only copies the frame bytes and parsed header into a POD and enqueues it;
  it touches no `std::vector` / `std::queue` / `m_stats`, and all stats, packet-log and pcab writing
  happen on the main-loop task that drains the queue, pcap included (grep-confirmed: no
  shared-container mutation
  remains reachable from the callback)

- **Given** the frame queue is full under a flood faster than the main loop drains it
- **When** the callback tries to enqueue another frame
- **Then** the frame is dropped and counted without blocking the WiFi driver task (zero block time)
  and without any heap corruption; capture keeps running

- **Given** the operator stops the capture or leaves the screen (`stopCapture`, `~SnifferScreen`)
- **When** teardown runs
- **Then** the promiscuous callback is disabled first, the queue is then drained to flush any
  remaining frames to the pcap file, and only then is the queue deleted, so no late enqueue can ever
  target a freed queue

## Design

**The callback runs in task context, not a true ISR.** The same promiscuous callback already calls
`EventBus::queue`, which takes a mutex with a 10 ms timeout (`event_bus.cpp:265`,
`xSemaphoreTake(queueMutex_, pdMS_TO_TICKS(10))`), and already calls heap-allocating
`std::vector::push_back` / `std::queue::push` without an immediate illegal-ISR-malloc abort. Both are
impossible from a hardware ISR. So the callback is the WiFi **driver task**, and the correct handoff
primitive is the blocking-capable task API `xQueueSend` used with a **zero** block time (never stall
the driver), not `xQueueSendFromISR`.

**One FreeRTOS queue of self-contained frames replaces the two raced std containers.** A new POD
`SniffedFrame` carries a copy of the frame bytes (capped at `SNIFFER_FRAME_CAP = 512`, the same cap
the retired `QueuedPacket` already truncated pcap frames to, so pcap output is unchanged) plus the
already-parsed header the main loop needs to classify without re-parsing: `length`, `rssi`,
`channel`, `timestamp`, `frameType`, `subtype`, `addr1/2/3`. The driver's frame buffer is valid only
for the duration of the callback, which is exactly why the bytes must be copied. A `QueueHandle_t
m_frameQueue` holds `SNIFFER_FRAME_QUEUE_DEPTH` of these. This retires `QueuedPacket`, the
`std::queue m_packetQueue`, `processPacketQueue`, and `MAX_QUEUE_SIZE`.

**Producer (WiFi task): `enqueueFrame`.** The capture callback wired in `startCapture` now points at
`enqueueFrame`, which copies the `CapturedPacket` into a `SniffedFrame` and `xQueueSend(..., 0)`s it,
incrementing a `m_droppedFrames` backpressure counter when the queue is full. It touches nothing
else. This is the whole of the WiFi task's work.

**Consumer (main loop): `drainFrameQueue` -> `processFrame`.** `update()` (main loop) drains up to
`SNIFFER_FRAME_DRAIN_BUDGET` frames per call via `xQueueReceive(..., 0)`, reconstructs a
`CapturedPacket` as a view over the `SniffedFrame`'s own (now main-loop-owned, stable) buffer, and
calls `processFrame`. `processFrame` is the former `onPacketReceived` body, unchanged in substance:
the same stats increments, the same `shouldCapturePacket` / tail-follow gating into
`addPacketToLog`, the same deauth-flood detection. The pcap write that used to live in
`processPacketQueue` folds in here (write + periodic 100-packet flush), since by this point we are
already on the main loop with the frame in hand. Because `render`, `drawStats`, `addPacketToLog` and
`processFrame` now all run on the one main-loop task, the std containers have a single accessor and
need no lock.

**Lifecycle ordering closes the teardown race.** `startCapture` creates the queue
(`xQueueCreate`) **before** `m_sniffer.start()`, so the queue exists whenever the callback can fire.
`stopCapture` calls `m_sniffer.stop()` **first** (it disables promiscuous mode and clears
`m_callback`, so no further enqueue can occur), then `drainFrameQueue()` to flush the tail into the
pcap file, then closes the pcap, then `vQueueDelete` + nulls the handle. `enqueueFrame` guards on a
null handle. This removes the exact free-ordering hazard the crash backtrace walked through.

**Sizing (named constants, no magic values).** `SNIFFER_FRAME_CAP = 512` (frame-byte cap, matches
the retired pcap truncation). `SNIFFER_FRAME_QUEUE_DEPTH = 16`: a busy 2.4 GHz channel delivers on
the order of a few hundred frames/s (~20 beacons every 100 ms plus data), i.e. well under one frame
per millisecond; against a worst-case ~10 ms gap between `update()` calls that is ~3 frames in
flight, so depth 16 is a >5x margin, and the exact drop rate under flood is observable on-device and
tunable. `sizeof(SniffedFrame)` ~= 544 B, so the queue is a single ~8.5 KB block allocated **once**
at `startCapture` and freed at `stopCapture` (against ~128 KB idle free, ADR/heap-audit numbers),
not per-packet; this is strictly less heap churn than the retired design, which grew a `std::deque`
and a `std::vector` from the hot path on every frame. `SNIFFER_FRAME_DRAIN_BUDGET = 16` (= depth) so
a full queue clears in one `update()` under normal load; residual frames carry to the next call
rather than blocking the loop.

**What existing code was considered.** (1) Mirroring the EventBus deferral pattern verbatim (a
`SemaphoreHandle_t` mutex around a `std::vector`, `event_bus.cpp:263-308`) was considered and
**rejected**: EventBus events are small fixed PODs, whereas these frames are ~544 B at hundreds/s, so
a growing mutex-guarded vector would reintroduce exactly the hot-path heap churn we are removing, and
taking a mutex per frame in the driver task adds contention. A FreeRTOS queue preallocates its fixed
storage and is the purpose-built task-to-task handoff, so it is the better-measured fit and is what
CLAUDE.md's "use FreeRTOS queues for task communication" rule points to. (2) The ISR-safe EventBus
`queue()` path from the same callback is **kept** and reused unchanged for the lightweight
`PACKET_CAPTURED` / `EAPOL_CAPTURED` notifications; this slice only moves the heavy screen-side work.
(3) Adding a lock around the std containers instead of moving the work was **rejected**: it keeps two
writers and pays a lock on every `render` and every frame, where moving to a single task removes the
need for a lock at all (the simpler fix, KISS).

**Explicitly out of scope.** The `PacketSniffer::m_stats` scalar counters (`packet_sniffer.cpp:384+`)
are also touched from the callback, but they are plain `uint32_t` with no heap behind them, so a
cross-task read is at worst a torn counter on the display, never heap corruption; left alone here.
The post-attack footer reading `[stop]` on a fresh non-capturing screen, and the undocumented
`+`/`-`/`h`/`m`/`s`/`c` keys, are separate issues tracked in the ledger (the latter folded into the
sniffer-screen redo feature).

## Verification

- `pio run -e cardputer` SUCCESS. `sniffer_screen.{h,cpp}` live under `ui/` and `modules/sniffer/`,
  neither of which is in the native `build_src_filter` allowlist, so this carries no host test, as
  recorded for the slice-0069..0077 UI work. The FreeRTOS queue types are used under `#ifdef ESP32`.
- `pio test -e native` stays green: the native-compiled sniffer policy is only
  `makeSnifferAttackTarget` (`test_sniffer_actions`, over the display-free `sniffer_types.h`), which
  this slice does not touch. Exact count recorded in As built.
- `node scripts/lint-docs.mjs` 0 errors.
- On-device DoD (the heap race only manifests on the dual-core hardware, so these are verified by the
  operator on the Cardputer): the reported repro run repeatedly no longer reboots or prints
  `CORRUPT HEAP`; capture under heavy traffic still fills the live log, advances the stats, and writes
  a valid pcap; stop/exit is clean. Numbers and sign-off recorded in As built.

## As built

Shipped as designed. The WiFi-driver-task callback no longer touches any shared container: it runs
`enqueueFrame`, which copies the frame into a `SniffedFrame` POD and `xQueueSend`s it (zero block
time, dropping and counting via `m_droppedFrames` when full) to a `QueueHandle_t m_frameQueue`. The
main loop's `update()` calls `drainFrameQueue`, which `xQueueReceive`s up to
`SNIFFER_FRAME_DRAIN_BUDGET` frames, rebuilds a `CapturedPacket` view over each POD's own buffer, and
runs `processFrame` (the former `onPacketReceived` body: stats, tail-gated `addPacketToLog`,
deauth-flood detection, plus the pcap write folded in from the retired `processPacketQueue`). The
`std::queue m_packetQueue`, the `QueuedPacket` struct, `processPacketQueue` and `MAX_QUEUE_SIZE` are
gone. `render`, `drawStats`, `addPacketToLog` and `processFrame` now all run on the one main-loop
task, so the std containers have a single accessor and the heap race is eliminated by construction.

- **DoD 1 (repro no longer reboots):** met on-device. The operator ran the exact reported sequence
  (sniff busy traffic, open packet log, select a packet, launch Deauth, ESC back, ENTER) repeatedly
  on the Cardputer with no reboot and no `CORRUPT HEAP` ("device check went perfect", 2026-10-19).
- **DoD 2 (callback touches no shared container):** met. `enqueueFrame` only copies into the POD and
  `xQueueSend`s; grep confirms no `m_packetLog` / `m_stats` / std-queue access remains reachable from
  the callback. All such work is in `processFrame`, called only from the main-loop drain.
- **DoD 3 (drop-on-full, no block, no corruption):** met. `xQueueSend(..., 0)` never blocks the
  driver task and increments `m_droppedFrames` on a full queue; capture keeps running, matching the
  old drop-on-full behaviour, and the on-device run under heavy traffic stayed stable.
- **DoD 4 (teardown ordering):** met. `startCapture` creates the queue before `m_sniffer.start()`;
  `stopCapture` calls `m_sniffer.stop()` first (disables promiscuous, clears the driver callback),
  then `drainFrameQueue()` to flush the tail into the pcap, then closes the pcap, then
  `vQueueDelete`s and nulls the handle. No late enqueue can target a freed queue.

Verification: `pio run -e cardputer` SUCCESS, Flash 74.6% (2,493,307 B of 3,342,336), static RAM
26.5% (86,704 B) unchanged from slice-0077 (the ~8.5 KB queue is a runtime heap allocation held only
while capturing); `pio test -e native` 844/844 (the change is entirely under `ui/`, which the native
`build_src_filter` excludes); `node scripts/lint-docs.mjs` 0 errors. On-device sign-off by the
operator 2026-10-19: the repro no longer corrupts the heap and normal capture (live log, climbing
stats, pcap to SD) is intact. The frame types are carried in the POD, so no re-parse is needed.
