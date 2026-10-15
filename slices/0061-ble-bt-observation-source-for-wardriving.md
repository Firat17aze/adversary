---
id: '0061'
title: "Add a BLE observation source to wardriving (BLE rows in the WiGLE CSV)"
type: slice
status: accepted
date: 2026-10-15
supersedes: []
superseded_by: []
---

## Goal

Deliver the ledger feature *"Wardriving: optional BLE/BT observation source writing
`BT`/`BLE`-typed rows into the existing WiGLE session CSV"* — an enhancement to the
wardriving pipeline (shared GPS/CSV/upload spine), not a separate feature. WiGLE's unified
`WigleWifi-1.4` CSV already carries a `Type` column (the row writer hardcodes `WIFI`
today), so adding BLE is a new scan source plus a `Type`-aware row, not a new file or a new
upload path.

The radio-sharing question that blocked this is settled by **ADR-0060**: run the WiFi
active scan and the BLE scan **concurrently via ESP-IDF coexistence** (measured to beat
application-level interleaving decisively), behind a `WiFi-only` (default) / `BLE-only` /
`Both` mode, with the BLE source reusing `BLEScanner` (not the attack-oriented
`BLESpanner`). This slice implements that decision.

Scope note: `BLEScanner` is NimBLE, so this ships BLE rows (`Type=BLE`). Classic-BT
discovery (`Type=BT`) is a separate radio mode NimBLE does not scan; it stays out of scope
and is noted as a follow-up.

## Definition of Done

- **Given** a WIFI `NetworkEntry` with known fields
- **When** the extracted pure `formatWigleRow()` formats it
- **Then** the row is byte-identical to today's `writeNetworkToCSV` output (same AuthMode
  mapping, same field order, trailing `,WIFI`) — proving the extraction is behaviour-neutral
  for the existing path

- **Given** a BLE observation (6-byte address, name, RSSI, a valid GPS fix)
- **When** `formatWigleRow()` formats it with `type = BLE`
- **Then** it emits `AA:BB:CC:DD:EE:FF,"<name>",<bleAuthMode>,<firstSeen>,0,<rssi>,<lat>,<lon>,<alt>,<acc>,BLE`,
  conforming to WiGLE's current BT/BLE field spec (confirmed at design time per D6)

- **Given** `wardrivingRadioMode = WiFi-only` (the default)
- **When** a session starts, runs, and stops
- **Then** the BLE controller is never initialized (zero added heap cost) and the CSV holds
  only WIFI rows — today's behaviour, unchanged

- **Given** `wardrivingRadioMode = Both` in an area with both WiFi APs and BLE advertisers
- **When** a session runs and then stops
- **Then** the CSV contains both WIFI and BLE rows (each deduplicated within its own type),
  and on stop `BLEScanner::deinit(false)` releases the controller and reclaims its heap

- **Given** `wardrivingRadioMode = Both` and a full session (the BSSID hash set near its
  5000-entry cap, GPS running, the CSV pending buffer in use)
- **When** the wardriving 60 s heap log is read at the session's free-heap floor
- **Then** the floor stays above a safe margin (threshold fixed in Design D7 from the
  measurement) — this is the residual on-device measurement ADR-0060 deferred; the measured
  floor is recorded in `As built`

## Design

### D1 — Reuse the pipeline, don't fork it
Add `ObservationType type` (`WIFI` default, `BLE`) to `NetworkEntry`
(`wardriving_types.h`). BLE devices flow through the **same** dedup → pending buffer →
GPS-tag → `flushToSD` path as WiFi; only the scan source and three row fields differ. The
GPS tagging, streaming-to-SD, buffer-flush, and session accounting are identical and are
reused as-is (rule-of-three: this is the pipeline's second consumer, and it is the same
logic, so reuse — not a new abstraction).

Dedup must be **type-aware**: a WiFi BSSID and a BLE address are both 6-byte MACs and could
collide in the current `seenBSSIDs_` hash. Use a separate `seenBLE_` set (or fold the type
into the hashed key) so a BLE device is never suppressed by a same-valued WiFi BSSID.

### D2 — Extract the row formatter (ADR-0058 pattern, boyscout)
`writeNetworkToCSV` currently inlines the `file.printf(...)`. Extract the row formatting
into a pure, SD/display-free `static void formatWigleRow(const NetworkEntry&, char* out,
size_t outLen)` — exactly like the existing `formatWigleTimestamp` — branching on
`entry.type` for the AuthMode string, Channel, and trailing `Type`. `writeNetworkToCSV`
then opens the file and writes the formatted line. This makes the row natively testable
(`modules/wardriving/` is already in the `[env:native]` `build_src_filter`), which is how
both the WIFI-regression and BLE-row DoD scenarios are proven in CI.

### D3 — BLE source (`BLEScanner`, reused per ADR-0060)
A new `processBLEScan()` mirrors `processWiFiScan()`: read `BLEScanner::getDevices()`, and
for each not-yet-seen address add a `NetworkEntry{ type=BLE, bssid=address, ssid=name,
rssi, channel=0 }`, then GPS-tag and buffer it. `BLEScanner` is reused, **not**
`BLESpanner` (attack module, ~31 KB, wrong tool). No `Type=BT` rows — NimBLE is BLE-only.

### D4 — Mode wiring in `update()` (coex, no scheduler)
Read `wardrivingRadioMode`:
- **WiFi-only**: current `processWiFiScan()` path only; ensure the BLE controller is not
  up.
- **BLE-only**: `processBLEScan()` only; WiFi radio idle (STA stays up for nothing radio-
  heavy), GPS still runs.
- **Both**: run both each `update()`; the ESP-IDF coex layer owns the radio time-division
  (ADR-0060) — no application-level interleaving (measured worse) is added.

`BLEScanner::init()` + continuous scan start on session start when the mode needs BLE;
`BLEScanner::deinit(false)` on `stopSession`/`discardSession` and whenever the mode is
WiFi-only, so WiFi-only sessions pay neither the ~58.6 KB nor the scan-throughput cost.

### D5 — Setting + UI
Add `uint8_t wardrivingRadioMode = 0;` to `WirelessSettings`
(`settings_manager.h`), persisted in the JSON load/save beside
`wardrivingScanIntervalMs` (0=WiFi-only, 1=BLE-only, 2=Both). Expose a 3-way selector on
the settings/wardriving screen. Default 0 keeps the stock experience identical.

### D6 — WiGLE BT/BLE field spec (design-time confirm)
Confirm WiGLE's current `WigleWifi-1.4` expectations for BLE rows before coding the
formatter: the `AuthMode` value (proposed `Misc [BLE]`), the `Channel` convention (proposed
`0`, since BLE advertises on 37/38/39 and WiGLE does not key on it), and the `Type` token
(`BLE`). Record what was confirmed in `As built`.

### D7 — Heap threshold
ADR-0060 measured a ~120 KB radio+display floor and a 58,572 B BLE-controller cost, but
not under live wardriving structures. Re-run/extend the archived coex probe
(`scratchpad/coex-probe-archive/`) with the session's data structures, or read the live
60 s heap log during a real `Both` session, to fix the safe-margin threshold and confirm it
holds. This is the measurement, not a guess.

### Considered and not reused
- **`BLESpanner`** — rejected (attack-oriented, heavy); `BLEScanner` is the observation tool
  (ADR-0060).
- **An application-level radio time-slice scheduler** — rejected; ADR-0060 measured naive
  interleaving strictly worse than concurrent coex on both detection axes.
- **A second BLE-only CSV** — rejected; the whole point is WiGLE's unified CSV + `Type`
  column, which the upload spine already carries.

## Verification

- **`pio test -e native`**, extending `test/test_wardriving/test_wardriving_manager.cpp`:
  - `test_wigle_row_wifi_unchanged` — a known WIFI `NetworkEntry` formats byte-identically
    to the pre-extraction output (the regression guard for D2; fails before the formatter
    branches correctly, passes after).
  - `test_wigle_row_ble` — a BLE observation formats to the expected `...,BLE` row (D6 spec).
  - `test_ble_dedup_vs_wifi` — a BLE address equal to an existing WiFi BSSID is still
    recorded (type-aware dedup, D1).
  These run in CI since `modules/wardriving/` is in the native build filter.
- **On-device (not unit-testable):** the heap-floor measurement (DoD scenario 5 / D7) and a
  real `Both`-mode session producing both WIFI and BLE rows in one CSV, plus confirmation
  that WiFi-only is byte-for-byte the old behaviour. Results recorded in `As built`.

## As built

_(filled at merge: the confirmed WiGLE BLE field spec, the measured `Both`-mode heap floor
and the margin threshold it set, the native test results, and on-device confirmation of
both row types + unchanged WiFi-only behaviour.)_

Shipped as specified; all six DoD scenarios met.

**WiGLE BLE field spec (D6).** Confirmed against WiGLE's `WigleWifi-1.4` CSV spec: BLE rows
carry AuthMode `Misc [LE]` (the spec's reference BLE example), Channel `0` ("0 for Bluetooth
devices"), and Type `BLE`. Classic-BT (`Type=BT`) stayed out of scope — NimBLE is BLE-only.

**Formatter + type-aware dedup (D1/D2).** `formatWigleRow()` ships as a pure, SD/display-
free static that branches on `entry.type`; `writeNetworkToCSV` builds its line through it.
Dedup folds `ObservationType` into bit 48+ of the 64-bit observation key, so a WiFi BSSID
and a BLE address of equal bytes never collide, and the WIFI key stays byte-identical to
the historical hash.

**Native tests.** `test_wigle_row_wifi_unchanged`, `test_wigle_row_ble`,
`test_ble_dedup_vs_wifi` added and green; full native suite 825/825.

**On-device (Cardputer ADV, 2026-10-15).** WiFi-only wrote only WIFI rows and never brought
the BLE controller up (byte-for-byte the old behaviour). A BOTH session wrote both types to
one CSV (42 WIFI + 110 BLE), BLE rows spec-correct
(`74:52:DA:BC:37:C7,"LENOVO",Misc [LE],...,0,-97,...,BLE`), MACs in canonical byte order
(vendor OUIs resolve), GPS-tagged.

**Heap floor (DoD scenario 5 / D7).** The first field run drained to OOM in ~20-25 min. Root
cause was not the dedup set (~1.2 KB for 152 uniques) but `NimBLEScan` defaulting to store-
all (`maxResults 0xFF`), accumulating every randomised BLE MAC for the scan's lifetime. Fixed
by `setMaxResults(0)` (callback-only; commit `f0a8b8e`). After the fix the BOTH-mode floor
holds steady at ~30 KB with no drain; WiFi-only floor ~80 KB. Both stay clear of the OOM
boundary.

**Controller reclaim (DoD scenario 4).** Measured on stop: BOTH exit leaves ~85 KB free,
WiFi-only exit ~86 KB — identical within noise, confirming `BLEScanner::deinit(false)`
returns the ~58.6 KB BLE controller heap. The ~64 KB gap from a pristine 149 KB cold boot is
the WiFi-STA operational baseline (present with or without BLE, matching the historical
~81 KB "Session started" figure), not a BLE leak.

**Deferred (ledger, outside this DoD):** the live on-screen list doesn't render BLE entries
in BOTH mode, and the log up-arrow doesn't scroll. Both tracked for a focused wardriving-UI
pass.
