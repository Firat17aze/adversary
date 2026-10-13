---
id: '0055'
title: "Remove the dead WardrivingExporter and fix the CSV timestamp it exposed"
type: slice
status: accepted
date: 2026-10-12
supersedes: []
superseded_by: []
---

## Goal

Close the ledger's `[bug] WardrivingExporter is dead code` (found during slice-0052's logging
pilot): `WardrivingExporter::exportToCSV`/`exportToJSON` have **no callers** anywhere in `src/`
or `test/`. The live export path is entirely the manager's own inline code — `writeNetworkToCSV`
streams one WiGLE CSV row per network as it scans, and `stopSession` writes a small session-summary
JSON. The exporter is not merely unreachable; it is actively unusable and wrong:

- **Architecturally incompatible.** It takes `const std::vector<NetworkEntry>&` — the *entire*
  result set in RAM — whereas the manager was deliberately built to stream to SD because, under
  the no-PSRAM invariant (~205 KB SRAM is the whole budget, CLAUDE.md), it "doesn't keep all
  networks in RAM". Wiring it in would reintroduce the exact full-RAM buffer the streaming design
  exists to avoid.
- **Buggy auth-mode table.** Its `securityToWigleAuthMode` disagrees with the canonical
  `WiFiSecurity` enum (`wifi_scanner.h`): it reads `4=WPA3, 5=WPA/WPA2, 6=WPA2/WPA3` and has no
  case `7`, where the enum is `4=WPA_WPA2, 5=WPA2_ENTERPRISE, 6=WPA3, 7=WPA2_WPA3`. The manager's
  inline mapping matches the enum; the dead one would mislabel security modes if revived.

The ledger line guessed the exporter "writes a WiGLE auth-mode CSV the manager's inline writer
may not" — but the manager *does* write a WiGLE `AuthMode` CSV, so that is not a reason to keep
it. The exporter's one genuine merit is real wall-clock timestamps via `TimeManager`, and chasing
that merit surfaced a real defect in the **live** path: `writeNetworkToCSV` stamps every row with
a **fabricated `"2024-01-01"` date**, filling only the time-of-day from `millis()`. That is worse
than useless for WiGLE import (a plausible-looking wrong date geotags observations to the wrong
day). So this slice **deletes** the dead exporter and **ports the real-time fix into the live
manager path** (boyscout: the merit is kept where it can actually run), with a unit test.

## Definition of Done

- **Given** the wardriving module after this slice
- **When** the tree is searched for `WardrivingExporter`, `exportToCSV`, `exportToJSON`
- **Then** `wardriving_exporter.cpp`/`.h` no longer exist, the manager no longer includes them,
  and all three builds (native/cardputer/m5stick) stay green

- **Given** a network first seen `elapsedMs` of device-uptime ago, while the clock is synced to a
  real epoch
- **When** the manager formats that network's WiGLE `FirstSeen` field
- **Then** it emits the correct real UTC `YYYY-MM-DD HH:MM:SS` for `now - elapsed`, not a
  hardcoded calendar date

- **Given** a network to stamp while the clock was never synced (`now()` returns 0)
- **When** the manager formats its `FirstSeen` field
- **Then** it emits the unambiguous epoch-zero sentinel `1970-01-01 00:00:00` rather than a
  plausible-looking fake date — "time unknown" is stated, not masked (global §3)

## Design

### D1 — Delete the exporter outright

Remove `src/modules/wardriving/wardriving_exporter.{cpp,h}` and the `#include "wardriving_exporter.h"`
line from `wardriving_manager.cpp`. Nothing else references the type (verified by grep over `src/`
and `test/`), and the native `build_src_filter` globs `+<modules/wardriving/>`, so dropping the file
simply removes it from every build. No replacement class is introduced — the streaming manager is
already the sole, correct export path, and a batch-mode exporter is the wrong abstraction for a
design whose whole point is to never hold the full set in RAM (global §3, no speculative generality).

### D2 — A pure, testable timestamp helper on the manager

The broken inline code reads `millis()` directly and fabricates the date. The fix extracts the
date math into a pure `static` function so it is unit-testable off-hardware (the existing
`test_wardriving` suite drives `millis()` through `setMockMillis`):

```cpp
// WardrivingManager, public static — pure, no millis()/TimeManager reads inside.
static void formatWigleTimestamp(uint32_t currentMillis, time_t currentEpoch,
                                 uint32_t eventMs, char* out, size_t outLen);
```

`writeNetworkToCSV` calls it with the live values `millis()`, `TimeManager::getInstance().now()`
and the entry's `firstSeenMs`, keeping the impure reads at the one call site and the logic pure.
The helper computes `eventEpoch = currentEpoch - (currentMillis - eventMs)/1000`; if that is a sane
epoch (≥ `MIN_SANE_EPOCH`, 2001-09-09, the point below which `now()`'s unsynced `0` falls) it
`strftime`s real UTC, otherwise it writes the epoch-zero sentinel `1970-01-01 00:00:00`. The
magic `2024-01-01` string and the `elapsed/millis` hand-rolling are deleted.

`MIN_SANE_EPOCH` is a named constant, not a bare literal (global §3); it is the same threshold the
dead exporter used, which is the only piece of that file worth carrying forward.

### D3 — Considered and not reused

- **Keep + wire in the exporter** — rejected per the Goal (full-RAM signature breaks the streaming
  invariant; wrong auth-mode table). Its real-timestamp merit is ported instead of preserved.
- **Also rewrite the summary-JSON path to per-network** — not done. The manager's session-summary
  JSON is an intentional low-RAM choice, not a defect; expanding it is out of scope and would need
  its own slice. Only the fabricated *timestamp* is a bug, and only it is fixed here.
- **Fix the timestamp in `wardriving_exporter` instead** — nonsensical: that file is being deleted
  and never runs.

## Verification

- **`pio test -e native`** gains two cases in `test_wardriving` exercising
  `WardrivingManager::formatWigleTimestamp` directly:
  - *synced*: `currentMillis=10000`, `currentEpoch=1700000006`, `eventMs=4000` ⇒ `eventEpoch=1700000000`
    ⇒ asserts `"2023-11-14 22:13:20"`. This **fails before** the fix (no such function existed; the
    inline code could only ever emit `2024-01-01 …`) and **passes after**.
  - *unsynced*: `currentEpoch=0` ⇒ asserts `"1970-01-01 00:00:00"` (sentinel, DoD scenario 3).
  The full suite (was 814 cases) stays green.
- **Dead-code removal** proven by grep returning only history, and by all three builds compiling
  without the file: `pio run -e cardputer`, `pio run -e m5stick`, `pio test -e native` green
  (DoD scenario 1).
- The CSV row-writing itself (SD I/O) remains device-only and is unchanged except for the one
  timestamp argument; no SD mock is invented for it.

## As built

Shipped as designed. `src/modules/wardriving/wardriving_exporter.{cpp,h}` were deleted and the
`#include "wardriving_exporter.h"` line removed from `wardriving_manager.cpp`; grep over `src/`
and `test/` confirms no remaining reference to `WardrivingExporter`/`exportToCSV`/`exportToJSON`
(DoD scenario 1).

The timestamp fix landed as the pure `static WardrivingManager::formatWigleTimestamp(currentMillis,
currentEpoch, eventMs, out, outLen)` (declared in the header, `<ctime>` added for `time_t`). It
reconstructs `eventEpoch = currentEpoch - (currentMillis - eventMs)/1000` and, when that is
`>= MIN_SANE_EPOCH` (named `constexpr`, 1000000000 = 2001-09-09), `strftime`s real UTC via
`gmtime_r`; otherwise it writes the `1970-01-01 00:00:00` sentinel. `writeNetworkToCSV` now calls it
with `millis()`, `TimeManager::getInstance().now()` and `entry.firstSeenMs`; the fabricated
`"2024-01-01"` snprintf and the hand-rolled hour/min/sec math were deleted.

**Verification run (2026-10-12):**

- `pio test -e native` — **816/816** passing (was 814; +2 new). The two added `test_wardriving`
  cases call `formatWigleTimestamp` directly: *synced* (`10000`, `1700000006`, `4000`) asserts
  `"2023-11-14 22:13:20"` (fails pre-fix — the inline writer could only emit `2024-01-01 …`);
  *unsynced* (`currentEpoch=0`) asserts `"1970-01-01 00:00:00"` (DoD scenarios 2 and 3).
- `pio run -e cardputer` — SUCCESS (1:52). `pio run -e m5stick` — SUCCESS (1:49). Both compile
  cleanly without the deleted file (DoD scenario 1).
- `node scripts/lint-docs.mjs` and `node scripts/check-immutable.mjs` green; `adr/INDEX.md`
  regenerated (53 decisions, only the 0055 row added).

**Noted, not fixed (boyscout → ledger):** the native build emits a pre-existing
`-Wformat-truncation` warning in `generateSessionId` (`snprintf` of `"WD-%04d%02d%02d-…"` into a
20-byte buffer that can need up to 71). It predates this slice and is unrelated to the export path,
so it is logged as its own `[bug]` rather than folded in here (global §3 scope discipline).
