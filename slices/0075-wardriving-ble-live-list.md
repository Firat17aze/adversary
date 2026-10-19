---
id: '0075'
title: "Show BLE observations in the wardriving live network log"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

In a BOTH-mode wardriving session the on-screen Network Log shows only WiFi, while the CSV
correctly records both WiFi and BLE (on device 2026-10-15 a session logged 110 BLE rows to
disk but none of them appeared in the live list). This closes the open ledger bug left out
of slice-0061's DoD: the manager harvests BLE advertisers through `processBLEScan` into the
same dedup/GPS/CSV pipeline (ADR-0060), but the *screen* builds its live ring buffer by
reading `WiFiScanner::getNetworks()` only and never reads `BLEScanner::getDevices()`, so a
BLE observation has no path to the display. Make the live list show BLE entries and mark
their type, so what the operator sees on screen matches what the session is writing.

Picked up with it: the live list's existing dedup is a trailing `strcmp` of the SSID against
the last five rows, which is wrong for a cumulative source and for unnamed advertisers. It
re-adds a row once the source holds more entries than the window (WiFi returns the full
current AP set each scan, so past five visible APs it duplicates), and it collapses every
unnamed BLE device (SSID `""`) onto a single row. The operator chose the root-cause fix
(2026-10-19): dedup by MAC across the whole visible ring for both sources, which shows each
distinct radio once and fixes the pre-existing WiFi >5 thrash in the same change.

## Definition of Done

- **Given** an active wardriving session in BOTH or BLE-only mode with BLE advertisers nearby
- **When** the operator opens the Network Log
- **Then** BLE observations appear as rows alongside (or instead of) WiFi, each tagged "BLE",
  so the live list reflects the same observations the CSV is recording

- **Given** a BLE row in the log
- **When** it renders
- **Then** it shows the device name (or the first three BD_ADDR octets when the advertiser is
  unnamed) and its RSSI, with a "BLE" type tag in place of the WiFi channel/security columns
  (channel 0 and WiFi security do not apply to a BLE advertiser, WiGLE convention)

- **Given** the same radio is seen repeatedly, and several unnamed BLE advertisers are present
- **When** observations are harvested into the display ring
- **Then** each distinct MAC (WiFi BSSID or BLE BD_ADDR) occupies at most one visible row:
  dedup is by `(type, bssid)` across the whole ring, so a long-visible WiFi AP is not re-added
  past the fifth row and unnamed BLE devices do not collapse onto one row

- **Given** a WiFi-only session
- **When** the log renders
- **Then** it shows WiFi rows as before (channel, security, RSSI) with no BLE rows, and a
  BLE-only session shows only BLE rows

## Design

The screen keeps its own display ring (`networkLog_`, 25 rows) rather than mirroring the
manager: the manager's `pendingNetworks_` is a transient staging buffer that `flushToSD()`
clears on every flush, so it is not a stable "recent observations" view. The fix is to feed
the ring from both scan sources the way the manager's ingest does, not to change the ring.

**Data source.** WiFi is harvested from `WiFiScanner::getNetworks()` as today. BLE is
harvested from `BLEScanner::getDevices()`, guarded by `BLEScanner::isInitialized()`: the
manager brings the NimBLE controller up in BLE/BOTH mode and tears it down in WiFi-only
(ADR-0060), so an initialized scanner is the signal that BLE is live this session, and the
screen needs no separate mode read. BD_ADDR bytes are reversed from NimBLE's little-endian
`getNative()` to canonical order, mirroring `processBLEScan` so the on-screen MAC matches the
CSV. The BLE harvest and the `ble_scanner.h` include are `#ifdef ARDUINO` (NimBLE is
device-only; `wardriving_screen.cpp` is excluded from the native build filter anyway).

**Dedup.** `LoggedNetwork` gains `uint8_t bssid[6]` and `wardriving::ObservationType type`.
One `appendObservation(name, bssid, rssi, channel, security, type)` helper replaces the two
would-be copies of the WiFi/BLE append (DRY, operator's root-cause choice), deduping by
`(type, bssid)` over the whole ring before writing. This is the actual fix for the cumulative
source: the pre-slice trailing-5 SSID window re-added any row the source still reported once
it had scrolled past the fifth slot (true for WiFi past five APs, and for BLE whose
`getDevices()` list only grows), and matched every unnamed BLE advertiser on `""`. A stable
MAC key over all 25 slots is cheap (<=25 compares) and shows each radio once.

**Row shape.** The row callback branches on `type`: a WiFi row is unchanged (SSID, `Ch%d`,
security, RSSI); a BLE row shows the name or a short MAC, a "BLE" tag in the channel column,
and RSSI, skipping the WiFi-only security column.

**Nameless advertisers.** Most BLE devices broadcast no name, and the scanner stamps those
with the literal placeholder `"Unknown"` (`ble_scanner.cpp`, `haveName()` is false), so a
`name.empty()` test never fires and the row would read "Unknown" instead of a MAC. The
placeholder is centralized as `adversary::BLE_NAME_UNKNOWN` with a `hasAdvertisedName(dev)`
predicate in `ble_scanner.h`; the harvest passes `nullptr` for a nameless device so the row
falls back to the first three BD_ADDR octets. This is the third site to key off the
placeholder (producer at `ble_scanner.cpp:163`, consumer at `ble_scanner_screen.cpp:286`), so
the magic literal is named once and `ble_scanner_screen`'s inline `== "Unknown"` compare is
refactored onto the shared predicate (rule of three, identical behavior). The scanner's CSV
output is deliberately untouched: the manager still records `"Unknown"` for nameless BLE,
which is slice-0061's field-tested WiGLE SSID column and out of this slice's scope.

**What existing code was considered.** `processBLEScan` (`wardriving_manager.cpp:427`) is the
reference for the BLE read (self-init, continuous scan, BD_ADDR byte-reversal) and the screen
mirrors its harvest shape rather than inventing one. The manager's `getPendingNetworks()` /
`getNetworks()` were considered as a single source for the whole list and rejected: both
return `pendingNetworks_`, which drains on flush, so the list would blink empty every flush
interval. `ObservationType` is reused from `wardriving_types.h` (not a new screen-local enum)
so the screen and the CSV speak the same discriminator. The multi-line-row and scroll-only
list work (ledger items 92-94) is unrelated; this slice does not touch `ScrollableList`.

## Verification

- `pio run -e cardputer` SUCCESS and fits: the change is screen render/harvest wiring reading
  the NimBLE and WiFi scanner singletons, device-only and not host-constructible, so it
  carries no host test, as recorded for the slice-0069/0072/0073/0074 UI work.
  `wardriving_screen.cpp` is outside the native `build_src_filter` (`-<ui/>`).
- `pio test -e native` stays green (844/844): no pure/host-compiled logic changed (the
  wardriving manager's native-tested ingest/dedup is untouched; the dedup edited here lives
  in the device-only screen).
- `node scripts/lint-docs.mjs` 0 errors.
- On-device DoD (the criteria the host cannot assert, verified by eye on the Cardputer):
  in a BOTH-mode session with BLE advertisers nearby the Network Log shows BLE rows tagged
  "BLE" alongside WiFi; unnamed advertisers show a short MAC and do not collapse; WiFi-only
  still shows WiFi rows only.

## As built

The live Network Log now harvests both scan sources. BLE advertisers are read from
`BLEScanner::getDevices()` in `WardrivingScreen::update()`, gated on `BLEScanner::isInitialized()`
(the manager's BLE/BOTH-vs-WiFi-only controller lifecycle, ADR-0060) and byte-reversed from
NimBLE's little-endian address to canonical BD_ADDR order, mirroring `processBLEScan`. The two
would-be append copies collapsed into one `appendObservation()` helper that dedups by
`(type, bssid)` across the whole 25-row ring, replacing the trailing last-5 SSID check: that
window re-added rows once the source held more than five entries (true for WiFi past five APs
and for BLE's cumulative `getDevices()` list) and collapsed every unnamed advertiser onto one
row. `LoggedNetwork` gained `bssid[6]` and `ObservationType type`; the row callback branches on
type, drawing a WiFi row unchanged (SSID, `Ch%d`, security, RSSI) and a BLE row as name-or-short-
MAC, a "BLE" tag in the channel column, and RSSI.

Not anticipated in the first cut: on device every BLE row read "Unknown". The scanner stamps
nameless advertisers (most of them) with the literal placeholder `"Unknown"`, so the harvest's
`name.empty()` fallback never fired. Fixed at the source of truth: the placeholder is named
`adversary::BLE_NAME_UNKNOWN` with a `hasAdvertisedName(dev)` predicate in `ble_scanner.h`; the
harvest passes `nullptr` for a nameless device so the row shows the first three BD_ADDR octets,
and `ble_scanner_screen.cpp:286`'s pre-existing inline `== "Unknown"` compare was refactored onto
the shared predicate (rule of three, identical behavior). The CSV path was deliberately left
alone: the manager still records `"Unknown"` for nameless BLE, slice-0061's field-tested WiGLE
SSID column.

Measured: `pio run -e cardputer` SUCCESS, Flash 74.6%, 2,493,023 bytes. `pio test -e native`
844/844 (no host-compiled logic changed; `wardriving_screen.cpp` and `ble_scanner.*` are outside
the native `build_src_filter`). `scripts/lint-docs.mjs` 0/0. On device 2026-10-19 the operator
confirmed BLE rows appear tagged "BLE" alongside WiFi in a BOTH-mode session, nameless advertisers
show a short MAC instead of a wall of "Unknown", and WiFi-only still shows WiFi rows only.
