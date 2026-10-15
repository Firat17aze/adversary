---
id: '0060'
title: "Wardriving scans WiFi and BLE concurrently via coexistence, not time-sliced"
type: architecture
status: accepted
date: 2026-10-15
supersedes: []
superseded_by: []
---

# ADR-0060 — Wardriving scans WiFi and BLE concurrently via coexistence, not time-sliced

## Context

The ledger carries a feature: add an optional BLE/BT observation source to wardriving,
writing `BT`/`BLE`-typed rows into the existing WiGLE session CSV (WiGLE's unified CSV
already carries a `type` column and its upload API accepts BT/BLE rows). The BLE-scan
half already exists (`BLEScanner`, `src/modules/ble/`); the open question — and the reason
the ledger demanded *"an ADR first"* — is how the single 2.4 GHz radio is shared between
the WiFi scan and the BLE scan on the ESP32-S3.

The ledger stated the premise as a near-certainty: *"promiscuous WiFi and a BLE scan can't
run simultaneously, so this is a measured scan-window-vs-detection-rate decision."* Both
clauses turned out to be wrong, and the correction is the substance of this decision:

1. Wardriving does **not** use promiscuous mode. It runs an **active** scan
   (`WiFiScanner::startScan()` → async `WiFi.scanNetworks`) on a ~1 s duty cycle — a
   full multi-channel sweep collecting beacons/probe-responses, not a promiscuous sniff.
2. WiFi and BLE scanning **can** run simultaneously under ESP-IDF software coexistence,
   and doing so measurably beats time-slicing.

Because the decision hinged on radio behaviour no amount of code-reading can settle, a
throwaway measurement spike (`probe/coex_probe.cpp`, `[env:probe_coex]`, since removed)
was flashed to the deployment Cardputer and driven through an RF-rich area for ~8 minutes:
four 2-minute phases, each resetting its unique-sighting sets so the per-phase count is a
detection-rate proxy. The spike mirrored the production radio stack (pioarduino platform +
Bruce esp32-arduino-libs + NimBLE 1.4.3, the same `BLEScanner` scan parameters) and brought
the display/canvas up, so its heap floor reflects the real UI memory load.

**Measured (2026-10-15, one drive):**

| phase        | wifi_uniq | ble_uniq | wifi_scans | ble_reports | min_free_heap |
|--------------|----------:|---------:|-----------:|------------:|--------------:|
| WIFI_SOLO    |        58 |        0 |         39 |           0 |       182,548 |
| BLE_SOLO     |         0 |       40 |          0 |          40 |       128,560 |
| CONCURRENT   |        33 |       49 |         13 |          49 |       124,696 |
| INTERLEAVED  |         2 |       13 |       5391 |         165 |       132,704 |

Heap deltas: boot (display up) 251,412 → after WiFi STA 192,372 → after BLE controller init
133,800. **WiFi STA costs ~59 KB; the BLE controller costs 58,572 B.** Lifetime free-heap
floor across the whole run: 122,772 B.

Reading the numbers:

- **Concurrent coexistence works and wins.** Both stacks ran together with a safe ~120 KB
  heap floor. BLE was *unharmed* (49 ≥ its 40 solo baseline — route variance, not a gain
  from concurrency). WiFi still delivered **33/58 ≈ 57 %** of its solo unique-AP yield
  (13 vs 39 completed scans). The ESP-IDF coex scheduler services BLE's short listen
  windows while preempting WiFi's long multi-channel sweeps, so WiFi bears the entire coex
  cost and BLE bears none.
- **Naive application-level interleaving (3 s WiFi / 3 s BLE) is strictly worse on both
  axes** — WiFi collapsed to 2 APs, BLE to 13. The `wifi_scans = 5391` tell explains why:
  a 3 s window is shorter than one full active-scan cycle, so deleting the in-flight scan
  at each switch (`WiFi.scanDelete()`) kills it mid-sweep and WiFi spins completing-empty.
  The cheap time-slice is a dead end.

## Decision

1. **Run the WiFi active scan and the BLE scan concurrently, relying on ESP-IDF software
   coexistence.** No application-level radio scheduler is written; the controller's coex
   layer owns the time-division.

2. **Reject application-level interleaving.** It was measured strictly worse than
   concurrent on both detection axes. This is recorded so it is not re-attempted: any
   future retry would need a `NimBLEDevice::deinit(false)` radio handoff (the pattern the
   firmware already uses to hand the radio to WiFi for TLS) plus windows far longer than
   one scan cycle — and would still have to beat concurrent's numbers, which it did not
   come close to here.

3. **Expose a three-way wardriving radio mode: `WiFi-only` / `BLE-only` / `Both`.**
   - `WiFi-only` is the **default** — it preserves today's behaviour and the full 100 %
     AP-yield baseline, and it keeps the BLE controller de-initialised so heap and WiFi
     throughput are unchanged for operators who don't want BLE.
   - `Both` is opt-in and accepts the measured WiFi-yield cost (~57 % of the solo rate per
     unit time) as a conscious trade for gaining BLE/BT rows. Continuous movement
     re-scans, so missed APs are partly recovered over a drive.
   - When `Both`/`BLE-only` is turned off, the BLE controller is released
     (`BLEScanner::deinit(false)`) so WiFi-only sessions pay neither the ~58.6 KB heap nor
     the scan-throughput cost.

4. **The BLE source reuses `BLEScanner`** (lightweight async, callback-driven,
   `src/modules/ble/`), **not** `BLESpanner` (the attack-oriented ~31 KB module). The
   wardriving sink consumes `BLEScanner`'s unique-device observations and writes
   `BT`/`BLE`-typed rows through the existing GPS/CSV/upload spine.

## Consequences

- **BLE/BT wardriving gains no new radio-scheduling code.** The controller's coex layer
  does the hard part; the implementing slice is a new `BLEScanner` consumer plus the
  CSV `type`-column rows and two settings (mode + enable), not new infrastructure.

- **WiFi AP yield drops ~43 % per unit time in `Both` mode.** This is the real cost and the
  reason the mode is a visible operator toggle rather than always-on. `WiFi-only` remains
  the default precisely so the stock wardriving experience is unchanged.

- **Heap: `Both` mode costs +58,572 B for the BT controller.** The probe's ~120 KB floor
  covered radio + display only. The live wardriving session adds its own structures (the
  BSSID hash set grows to ~40 KB at the 5000-AP cap, plus GPS state and the CSV pending
  buffer), so the **slice must re-measure the free-heap floor under a full session with BLE
  enabled** and confirm a safe margin before shipping `Both` on. The 58,572 B BLE delta is
  the transferable number; wardriving already logs heap every 60 s, which is the instrument.
  TLS upload headroom (~62 KB) is not needed concurrently — upload is a separate
  post-session step, not in-flight during the scan.

- **The measurement spike is throwaway** and is removed (file + `[env:probe_coex]`) once
  this ADR lands; it never enters git. Its numbers live here, which is the record.

- **The ledger feature line stays open** (this ADR clears its "needs an ADR first" blocker;
  the BLE-into-wardriving wiring is now a schedulable slice) and is updated to cite this ADR.
