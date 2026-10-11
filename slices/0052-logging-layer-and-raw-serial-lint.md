---
id: '0052'
title: "Logging layer and raw-serial lint"
type: slice
status: accepted
date: 2026-10-11
supersedes: []
superseded_by: []
---

## Goal

Deliver the mechanism ADR-0051 decided: a thin, compile-time level-gated logging layer
(`src/utils/log.h`) that every log line will route through, plus the CI lint that stops the
raw-`Serial.print*` sprawl from growing while the existing 583 sites migrate incrementally.
This is the **first** slice against the ledger's open `[deferred] Standardize logging` item;
it ships the layer, the lint, retires the dead scaffolding, converts **one pilot module**
(the two wardriving files, the only ones already carrying ad-hoc `LOG_` macros), and carries
the **flash-delta probe** ADR-0051 named — the probe needs the layer plus one converted
module to measure honestly. The remaining ~60 files migrate in follow-up slices under the
same ledger item; this slice does **not** touch them.

## Definition of Done

- **Given** a translation unit built for native at `ADV_LOG_LEVEL=ADV_LOG_INFO`
- **When** it calls `LOG_E("T","e")`, `LOG_W("T","w")`, `LOG_I("T","i")` and `LOG_D("T","d")`
- **Then** the error, warning and info lines are emitted (captured in `g_mockSerialLog`,
  each carrying its level tag and the `"T"` module tag) and the debug line emits nothing —
  proving info is the inclusive boundary and anything above it is compiled out

- **Given** a translation unit built at `ADV_LOG_LEVEL=ADV_LOG_ERROR` (the quietest non-silent level)
- **When** it calls all four macros
- **Then** only `LOG_E` emits; `LOG_W`/`LOG_I`/`LOG_D` emit nothing — proving an error is
  never stripped (fail-loud preserved) while warnings and below are gated away

- **Given** a source file that introduces a raw `Serial.print`/`println`/`printf` outside the
  sanctioned sinks (`src/utils/log.h`, `src/utils/error_logger.h`) and is not in the lint
  baseline
- **When** `node scripts/lint-raw-serial.mjs src` runs
- **Then** it exits non-zero naming the file and line; **and given** the current 62 legacy
  files recorded in the baseline it exits zero; **and given** a baseline entry whose file no
  longer contains any raw `Serial.print*` it exits non-zero demanding the stale entry's removal
  (the baseline may only shrink)

- **Given** the two wardriving files
- **When** the firmware and the native suite build
- **Then** they contain no local `#define LOG_I/W/E`, route through `utils/log.h`, and
  `test_wardriving`'s existing assertions stay green

- **Given** `src/config/config.h` after this slice
- **When** the tree is grepped and built
- **Then** `DEBUG_ENABLED`, `DEBUG_LOG` and `LOG_TAG` are gone (0 references, already 0 uses)
  and every env still compiles

## Design

### D1 — `src/utils/log.h`: the layer

Four variadic macros `LOG_E/W/I/D(tag, fmt, ...)`. Level constants
`ADV_LOG_NONE=0, ADV_LOG_ERROR=1, ADV_LOG_WARN=2, ADV_LOG_INFO=3, ADV_LOG_DEBUG=4`. Each macro
is gated by `#if ADV_LOG_LEVEL >= ADV_LOG_<LEVEL>`; below that threshold the macro expands to
`do {} while (0)`, so **the format string never reaches rodata** — this is the flash-recovery
mechanism. A macro (not a function) is required: only text the preprocessor removes is absent
from the image; a runtime `if` would keep the string.

Output goes through **`Serial`**, not bare `printf`. On device `Serial` is the UART; on native
the test harness already links a `MockSerial Serial` that captures to `g_mockSerialLog`
(`test/common/arduino_mocks.*`), so the same code path is **unit-testable** by assertion. This
is a deliberate, documented refinement of ADR-0051's looser "map to printf on native" — the
mock keeps tests building *and* makes the gate assertable; bare `printf` would bypass the
capture. Each line is prefixed `"[E] <tag>: "` etc. so grep/tag filtering works and the
per-call `tag` replaces the retired global `LOG_TAG "ADV"`.

`LOG_E` additionally **tees to the existing SD `ErrorLogger`** (`adversary::logger::logError`),
making error persistence the default instead of today's 3-site accident. The SD append lives
in `error_logger.h`'s `#ifdef ESP32` branch (its native path is `printf`, unchanged by this
slice), so the persistence half is **device-verified**, not unit-tested — consistent with how
every other SD path in this tree is proven. The unit test asserts the Serial half of `LOG_E`.

**Considered and not reused:** ESP-IDF's `ESP_LOGx` — rejected by ADR-0051 (couples the gate to
IDF semantics, absent on native). The ad-hoc wardriving `LOG_I/W/E` macros are the seed of this
layer, not a thing to keep — they are retired into it (D3).

### D2 — `ADV_LOG_LEVEL` default and the release tradeoff

`-DADV_LOG_LEVEL=ADV_LOG_INFO` is added to `[env_embedded]`, the m5stick flags, and
`[env:native]`. **Default = INFO is a no-behaviour-change choice this slice**: nothing currently
logged above info exists, so today's on-device serial output is unchanged, preserving the
operator's live-serial debugging (there is only one embedded env — `release.yml` builds
`pio run -e cardputer`, the very env flashed for development, so lowering the default would strip
info from the build debugged on-device).

The **actual release strip** (lowering the shipped level, or splitting a release env) is
deliberately *not* done here — it trades away dev serial visibility and is only worth it once
migration is broad. It stays the open ledger migration item. What this slice delivers is the
*mechanism* plus a *measured* proof of the payoff (D5), not the payoff itself. This is the one
choice beyond ADR-0051's settled axes; flagged for operator confirmation at review.

### D3 — Retire dead scaffolding, convert the pilot

`config.h`: delete the `DEBUG_ENABLED` block and `#define LOG_TAG "ADV"` (0 uses, confirmed).
`wardriving_manager.cpp` (20 sites) and `wardriving_exporter.cpp` (9 sites): delete the 5 local
`#define LOG_*`, `#include "utils/log.h"`, and pass a `"WARDRV"` tag. These two files are in the
native `build_src_filter`, so `test_wardriving` exercises the converted calls — the pilot is
test-covered, which is why it was chosen over a device-only module.

### D4 — `scripts/lint-raw-serial.mjs`: the ratchet

Zero-dependency `.mjs` modelled on `scripts/lint-serial-escapes.mjs` (same scan/export shape,
same CI style). It flags `Serial.print`/`println`/`printf` call sites, **exempting the two
sanctioned sinks** (`src/utils/log.h`, `src/utils/error_logger.h`) by path. A JSON baseline,
`scripts/raw-serial-baseline.json`, lists the legacy files permitted to still contain raw
Serial (the 62 today). The lint fails when: (a) a non-baselined file contains raw Serial — a
*new* site, blocked; or (b) a baselined file contains **none** — a stale entry that migration
must delete, so the baseline only ever shrinks.

**File-set, not per-site counts, by design.** A count-per-file baseline would churn on any
unrelated edit that splits or joins a `printf` and fire spurious CI failures across 62
actively-developed files; the maintenance tax is not worth the marginal strictness. Migration
converts a file wholesale (it leaves the baseline entirely), so file-granularity matches how the
work actually lands. The residual gap — adding a raw line to an already-legacy file passes — is
accepted and called out here; it is caught at review, and the gap closes file-by-file as the
baseline shrinks to empty. Wired as a new CI job `raw-serial` beside `serial-escapes`, running a
`node --test` unit test and then the lint over `src`.

### D5 — Flash-delta probe (measured, per ADR-0051)

With the pilot converted, build `cardputer` twice: `-DADV_LOG_LEVEL=ADV_LOG_INFO` (pilot info
kept) vs `-DADV_LOG_LEVEL=ADV_LOG_WARN` (pilot info/debug stripped). The flash delta isolates the
rodata+code the gate removes for the pilot's info/debug sites, yielding a **measured per-site
figure** to extrapolate transparently across the 583 total sites — replacing the ~24 KB
literal-rodata *floor* (ADR-0051 context) with a real marginal number. Recorded in As built.

## Verification

- **Native `test_log`** (new, built at `ADV_LOG_LEVEL=ADV_LOG_INFO`): asserts `LOG_E/W/I` each
  append their tagged line to `g_mockSerialLog` and `LOG_D` appends nothing. **Fails before:**
  `utils/log.h` does not exist.
- **Native `test_log_quiet`** (new, built at `ADV_LOG_LEVEL=ADV_LOG_ERROR` via a per-dir build
  flag): asserts only `LOG_E` emits. Proves errors are never stripped and warn/info/debug are.
  Two dirs because the gate is compile-time — one `ADV_LOG_LEVEL` per translation unit.
- **`scripts/lint-raw-serial.test.mjs`** (new, `node --test`): drives the scan + baseline
  evaluation over fixtures — a new file with raw Serial fails; a baselined file passes; a
  sanctioned-sink path is exempt; a stale (now-clean) baseline entry fails. **Fails before:**
  the lint does not exist.
- **Existing `test_wardriving`** stays green after the pilot conversion (no assertion changes) —
  proves the macro swap is behaviour-preserving.
- **Build:** `pio run -e cardputer` and `pio run -e m5stick` green; `pio test -e native` full
  suite green. Cardputer RAM/Flash and the D5 probe delta recorded in As built.
- **On-device (Cardputer, serial):** a `LOG_E` both prints to serial **and** appends an
  `[ERROR]` record to `/adversary/logs/error.log` (the tee's SD half, which is ESP32-only and
  cannot be asserted natively); wardriving info lines still appear at the default INFO level.

## As built

`src/utils/log.h` ships the four macros `LOG_E/W/I/D(tag, fmt, ...)`, gated by `ADV_LOG_LEVEL`
(ordinals `ADV_LOG_NONE=0` … `ADV_LOG_DEBUG=4`); a macro above the level expands to
`do {} while (0)` so its format string leaves rodata. Output routes through `Serial` — on
native the `MockSerial` that captures to `g_mockSerialLog`, which is a documented tightening of
ADR-0051's "printf on native" (the mock keeps the gate unit-testable). `LOG_E` is target-split:
on device it formats into a 160-byte stack buffer (`ADV_LOG_ERROR_MSG_MAX`) and tees to
`adversary::logger::logError` (one UART line + the SD `/adversary/logs/error.log` append); on
native, which has no SD, it prints the same `[ERROR] tag: msg` line to the captured Serial. This
split avoided touching `error_logger.h` and avoided double-printing on device. `tag` and `fmt`
must both be string literals (compile-time concatenation, the retired wardriving idiom).

Retired the dead `config.h` block (`DEBUG_ENABLED`/`DEBUG_LOG`/`LOG_TAG "ADV"`, 0 uses). Pilot:
`wardriving_manager.cpp` (20 sites) and `wardriving_exporter.cpp` (9 sites) dropped their 5 local
`#define LOG_*` and route through `utils/log.h` under tags `WARDRV`/`EXPORT`; the inline
`[Wardriving]`/`[Exporter]` message prefixes folded into those tags. `platformio.ini` adds
`-DADV_LOG_LEVEL=ADV_LOG_INFO` to `env_embedded`, `m5stick` and `native` (INFO = no behaviour
change). `scripts/lint-raw-serial.mjs` + `raw-serial-baseline.json` (59 legacy files, wardriving
dropped out) + `lint-raw-serial.test.mjs` (7 cases) + CI job `raw-serial` implement the file-set
ratchet (blocks new raw Serial outside `log.h`/`error_logger.h`; flags stale baseline entries).

**Definition of Done — met:**
- Gate boundaries proven by `test_log` (INFO: E/W/I emit, D stripped) and `test_log_quiet`
  (ERROR: only E emits) — errors never strip (fail-loud intact).
- Ratchet proven by `lint-raw-serial.test.mjs`: new raw-Serial file fails, baselined file passes,
  sanctioned sinks exempt, stale entry fails; `node scripts/lint-raw-serial.mjs src` exits 0.
- Pilot conversion behaviour-preserving: `test_wardriving` green unchanged; no local `LOG_` left.
- `config.h` scaffolding gone (0 references, build green).

- Native: full suite **814/814** (was 811; +`test_log` ×2, +`test_log_quiet` ×1). `serial-escapes`
  stays green after the wardriving edits.
- `pio run -e cardputer` **SUCCESS** — RAM 26.5% (86704 B, unchanged), Flash **2483575 B** at the
  default INFO (+48 B vs slice-0050's 2483527 B: the layer's `LOG_E` buffer/tee path, negligible).
- **D5 flash-delta probe (measured, single-definition WARN build, pilot objects force-recompiled):**
  stripping the pilot's 14 info sites drops cardputer flash to **2482675 B — 900 B, ~64 B/info-site
  — with zero source change**, purely the compile-time gate. This is the real marginal number
  ADR-0051 asked for, replacing its 24 KB literal-rodata *floor*. Transparent extrapolation: the
  pilot was ~48 % info-level (14/29); if the 583 total sites migrate at a similar mix and ship at
  WARN, the recoverable flash is on the order of ~15–18 KB — an extrapolation from one module, not
  a measurement, and the per-module figure will be re-measured as slices land.
- `pio run -e m5stick` **SUCCESS** — RAM 21.0% (68768 B), Flash 77.2% (2428887 B), still clear of
  the 3 MB OTA-less `huge_app.csv` ceiling.
- **On-device (Cardputer ADV, 2026-10-11):** both device-only claims verified.
  - *info-at-INFO:* a live wardriving session emitted the `WARDRV`-tagged info stream through
    the layer on real serial — `[INFO] WARDRV: Session started …`, `Found N new networks`,
    `Flushed N networks to SD`, `Session stopped`, `Session summary saved` — confirming
    `LOG_I` survives at the shipped default.
  - *`LOG_E` SD tee:* verified by a throwaway probe (one session-start `LOG_I` temporarily
    promoted to `LOG_E`, flashed, then reverted and the clean INFO image re-flashed — the
    committed tree is unchanged). The probe both printed `[ERROR] WARDRV: Session started:
    WD-20260923-112234 …` to serial **and** appended the matching record to the SD at
    `/adversary/logs/error.log` carrying the SD-only `| heap=81232` suffix, proving the tee
    routes through `error_logger::logError` to both sinks on device.
  - *finding:* the `EXPORT`-tagged sites could not be fired — `WardrivingExporter::exportToCSV`
    /`exportToJSON` have no callers in `src/` (the manager streams to CSV and writes its own
    JSON summary), so that class is dead code. Logged as a `[bug]` in `LEDGER.md` for a
    follow-up; out of scope for this slice.
