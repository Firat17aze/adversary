---
id: '0053'
title: "Migrate raw Serial to the logging layer"
type: slice
status: accepted
date: 2026-10-11
supersedes: []
superseded_by: []
---

## Goal

Empty the raw-`Serial.print*` ratchet. slice-0052 shipped the layer (`src/utils/log.h`,
`LOG_E/W/I/D`), the CI `raw-serial` lint, and a pilot conversion; it left the ledger's
`[deferred] Standardize logging` item open with its concrete remainder: the **574 raw-Serial
sites across the 59 files** recorded in `scripts/raw-serial-baseline.json` still bypass the
layer. This slice converts all of them, deleting each file from the baseline as it is
converted, until the baseline is empty and the lint's only sanctioned sinks are `log.h` and
`error_logger.h`. Converting every site is also what finally unlocks the deferred release
strip (lowering the shipped `ADV_LOG_LEVEL`): the flash win ADR-0051 is for cannot be banked
while 574 info/debug format strings still ship unconditionally. This slice closes the
migration half of the ledger item; the release-strip and the dead-`WardrivingExporter`
`[bug]` remain their own open lines.

The work lands in **subsystem-grouped commits** on `feature/raw-serial-migration`, each
removing its files from the baseline and leaving every build green — so the branch is
bisectable and the ratchet tightens monotonically. The pilot (wardriving) is already done and
out of the baseline; this slice does not revisit it.

## Definition of Done

- **Given** the 59 files in `scripts/raw-serial-baseline.json` and the whole `src` tree
- **When** every file has been converted and `node scripts/lint-raw-serial.mjs src` runs
- **Then** the baseline JSON is `[]` (empty), the lint exits zero, and the only files
  containing a raw `Serial.print*` are the two sanctioned sinks `src/utils/log.h` and
  `src/utils/error_logger.h`

- **Given** a converted translation unit built at the default `ADV_LOG_LEVEL=ADV_LOG_INFO`
- **When** the firmware and native suite build
- **Then** `pio run -e cardputer`, `pio run -e m5stick` and `pio test -e native` are all
  green — proving no site dropped a needed value, broke a non-statement use of a `printf`
  return, or lost its format arguments in the conversion

- **Given** a site whose first argument was a runtime value, not a literal (e.g.
  `Serial.println(someString)`)
- **When** it is converted
- **Then** it routes through `LOG_x("TAG", "%s", value)` with a literal format string — never
  a format built from a runtime value — so the layer's literal-only contract holds and no
  value is silently interpreted as a format

## Design

### D1 — Level policy (the per-site judgement the lint cannot make)

Each raw site maps to exactly one level by what the line *means*, not by its current
punctuation. The policy, applied uniformly so grep/level-filtering stays meaningful:

- **`LOG_E`** — an operation failed or an invariant broke: a failed open/init/alloc, a caught
  exception, a parse/transport error that aborts the path. These additionally tee to the SD
  `ErrorLogger` (slice-0052 D1), so promoting a real failure to `LOG_E` is a feature: it now
  persists. A site is `LOG_E` only if it marks an actual failure — not merely because the old
  code printed it loudly.
- **`LOG_W`** — a recoverable anomaly or fallback: "not found, continuing", retry, a degraded
  path taken. The operation proceeds.
- **`LOG_I`** — normal lifecycle/status the operator wants on live serial at the shipped
  default: start/stop, counts, state transitions, "saved N to SD".
- **`LOG_D`** — verbose per-packet / per-iteration / heap-dump trace that would drown the
  INFO stream. Everything currently gated behind an ad-hoc debug flag, and the heap
  instrumentation the ledger already wants off-by-default, lands here.

### D2 — Mechanical shape, and the three non-mechanical cases

The regular case is a transcription: `Serial.printf("[TAG] msg: %d\n", x)` →
`LOG_I("TAG", "msg: %d", x)` — the embedded `[TAG]` prefix becomes the log tag (uppercased,
one stable tag per subsystem), and the trailing `\n` is dropped because every macro appends
one. `Serial.println("[TAG] msg")` with no args → `LOG_I("TAG", "msg")`.

Three cases need real rework and are **not** sed-safe, called out so review can check them:

1. **Non-literal first argument** — `Serial.println(str)` / `Serial.print(value)` has no
   format literal. It becomes `LOG_x("TAG", "%s", str.c_str())` (or the matching specifier
   for a numeric/`const char*` value). The format stays a literal (DoD scenario 3).
2. **Piecewise lines** — a run of `Serial.print(a); Serial.print(b); Serial.println(c);`
   building one line across several calls. Because each macro appends its own `\n` and its
   own `[level] tag:` prefix, these are **consolidated into a single `LOG_x`** with one
   format string, not converted one-to-one (which would fragment one line into three tagged
   lines). This is the one place the conversion changes the serial byte stream, by design.
3. **Return-value / expression use** — a `Serial.printf(...)` whose return value is read, or
   used anywhere other than as a bare statement, cannot become a `do {} while (0)` macro. The
   build catches these (DoD scenario 2); each is rewritten to compute the value without the
   macro. Expected to be rare-to-none, but the compile is the backstop.

### D3 — Tag catalogue

Tags come from the `[Bracketed]` prefixes already in the strings, normalised to one short
uppercase tag per subsystem (e.g. `[BLE]`→`BLE`, `[RawWiFi]`→`RAWWIFI`, `[NMEA]`→`NMEA`,
`[Logger]`-style HAL prefixes→their module). Where a file's sites use several ad-hoc
prefixes, they collapse to the file's one subsystem tag so a level+tag grep selects a
coherent stream. No new tag taxonomy is invented; this is normalisation of what the strings
already say.

### D4 — Commit grouping and the ratchet

One commit per subsystem group (`modules/ble`, `modules/gps`, `modules/network`,
`modules/wifi`, `modules/attack`, `modules/storage`, `modules/rfid`, `hal`, `ui`, `utils`,
`core`, `main`), ordered smaller-and-leafier first. Each commit converts its files **and**
removes exactly those paths from `scripts/raw-serial-baseline.json`, so the lint stays green
at every commit (no stale entries, no new sites) and the baseline shrinks monotonically. No
commit mixes a conversion with an unrelated change.

**Considered and not reused:** a scripted mass-replace (sed/codemod) — rejected. The level
choice (D1) and the three rework cases (D2) are judgement per site, not pattern substitution;
a codemod would hard-code `LOG_I` everywhere and silently mangle the non-literal and piecewise
cases. The existing `scripts/lint-raw-serial.mjs` ratchet is reused unchanged as the
gate — this slice only shrinks its baseline, it does not touch the lint.

## Verification

- **`scripts/lint-raw-serial.mjs`** over `src` exits zero with an **empty** baseline at the
  end of the migration; at every intermediate commit it also exits zero (converted files
  removed from the baseline, none left stale, no new site introduced). **Fails before** each
  commit: the just-converted file is still listed, or still contains raw Serial, so the lint
  reports either a stale entry or an unconverted site. This is the regression proof for a
  print-site migration — there is no per-line unit assertion for 574 transcriptions, and
  inventing 574 serial-capture tests would be the fragile, duplicate testing the bar forbids;
  the lint + the compile are the observable gate.
- **Build:** `pio run -e cardputer`, `pio run -e m5stick`, and `pio test -e native` (full
  suite) green after each subsystem commit and at the end — the compile is what proves no
  dropped value, lost argument, or expression-context `printf` survived (DoD scenarios 2, 3).
- **Existing native suite** stays green throughout (no assertion changes) — the files in the
  native `build_src_filter` have their converted calls exercised by their existing tests,
  proving the swap is behaviour-preserving there.
- **On-device (Cardputer ADV, serial):** spot-check one converted subsystem per wave on real
  serial — its `LOG_I` lifecycle lines still appear at the default INFO level, and a forced
  failure in a converted path writes an `[ERROR]` record to `/adversary/logs/error.log` (the
  `LOG_E` SD tee, ESP32-only, not assertable natively). Recorded per wave in As built.

## As built

_Filled in at merge, per wave. Records the final baseline state, the per-subsystem tag
assignments and level splits, the cardputer/m5stick/native build figures, and the on-device
spot-checks — confirming each Definition-of-Done scenario was met or recording the deviation._

Every raw `Serial.print*` site recorded in `scripts/raw-serial-baseline.json` was converted
to `LOG_E/W/I/D`. The baseline is now `[]`; `node scripts/lint-raw-serial.mjs src` exits zero,
and the only files still calling `Serial.print*` are the two sanctioned sinks `src/utils/log.h`
and `src/utils/error_logger.h` (**DoD scenario 1 met**). `Serial.begin(115200)` survives in
`main.cpp` — it configures the port, it is not a log site, and the lint's regex does not match
it.

The work landed in **twelve subsystem-grouped commits** on `feature/raw-serial-migration`,
leaf-first, each removing exactly its files from the baseline so the ratchet tightened
monotonically and every commit stayed green:

| # | Commit | Subsystem group | Principal tags |
|---|--------|-----------------|----------------|
| 1 | `931d42a` | `modules/ble`, `modules/gps` | BLE, BLESPAM, GPS, NMEA |
| 2 | `d8efac1` | `modules/rf`, `modules/ir` | CC1101, RF, IR |
| 3 | `2b7c95b` | `modules/network` | NET and subsystem tags |
| 4 | `5a546b7` | `modules/wifi` | WIFI, SCAN, STA |
| 5 | `32db108` | `modules/attack` | DEAUTH, EVILTWIN, KARMA |
| 6 | `66b769d` | `modules/storage`, `modules/rfid` | REGISTRY, RFID |
| 7 | `b426d0a` | capture, pcap, sniffer, server, captive portal | CAPTURE, PCAP, SNIFFER, SERVER, PORTAL |
| 8 | `0ca1120` | `modules/system`, `core` events | SYS, EVENTBUS |
| 9 | `34cb537` | `hal` expansion/input/storage | CC1101, NRF24, CAP, INPUT, LITTLEFS, SDMGR |
| 10 | `868d687` | UI framework + simpler screens | SCREEN, BEACON, BLESCAN, RADIO, RFID, … |
| 11 | `e1f9e61` | capture-heavy UI screens | CAPTURES, HS/AUTOHUNT/PMKID, SCANNER, SNIFFER |
| 12 | `15c6409` | `main.cpp` + remaining `utils` | MAIN, MODULES, INIT, UI, INPUT, HSSAVE, HC22000, PMKIDSAVE, SHOT, RAWWIFI |

Per-site levels followed D1 uniformly across all waves. Representative splits from the final
wave (`main.cpp` + utils): start/stop lifecycle, boot banner, detect results and save
successes are `LOG_I`; per-packet TX trace (`raw_wifi`), the 5-second loop heartbeat, GPS
telemetry and the per-keypress/per-action dumps are `LOG_D` (stripped at the INFO default);
input-validation rejects (`Unknown action`), the degraded no-SD boot and the canvas-restore
escalation stages are `LOG_W`; canvas create/restore failure, directory-create and file
open/write failures are `LOG_E` (teed to the SD error log on device). Tags were normalised
from the existing `[Bracketed]` prefixes, one stable tag per subsystem (D3).

All three non-mechanical cases from D2 were exercised. Non-literal first args were rewritten
to `LOG_x("TAG", "%s", value)` with a literal format (DoD scenario 3 — e.g. the earlier
network/wifi waves). Piecewise lines were consolidated into one call: in the final wave the
six-call boot banner collapsed to one `LOG_I` with embedded `\n`s, and the two-print
canvas-restore failure diagnostic collapsed to one `LOG_E`; the earlier radio/SD-ladder waves
did the same for their edge-sample and frequency-ladder prints. No return-value/expression
use of a `printf` survived — the compile was the backstop and flagged none. LOG_D compile-out
hazards (a value read only by a `LOG_D` becoming unused at INFO) were handled by inlining the
value into the call or marking the local `[[maybe_unused]]`; a final forced recompile of the
touched TUs showed **no warnings** in any converted file.

**Build (DoD scenario 2 met):** at the default `ADV_LOG_LEVEL=ADV_LOG_INFO`, all three targets
are green at the final commit —

- `pio test -e native`: **814/814** test cases succeeded (no assertion changes across the
  migration; the native-filtered converted TUs — `sd_manager.cpp`, `raw_wifi.cpp`,
  `screenshot.cpp`, and the `modules`/`core`/`utils` set — are exercised by their existing
  suites, proving the swap behaviour-preserving there).
- `pio run -e cardputer`: SUCCESS — Flash 74.1% (2 476 663 B), RAM 26.5% (86 704 B).
- `pio run -e m5stick`: SUCCESS — Flash 77.0% (2 421 719 B), RAM 21.0% (68 768 B).

**Deviation — on-device serial spot-check (Verification bullet 4) not performed.** The
Cardputer-ADV serial check (confirming `LOG_I` lifecycle lines still print at INFO and a forced
failure writes an `[ERROR]` record to `/adversary/logs/error.log` via the ESP32-only `LOG_E`
SD tee) requires physical hardware not available in this environment. The SD-tee path itself is
unchanged from slice-0052, which device-verified it; this slice only re-routed call sites
through it. Left as the one open on-device confirmation before the release-strip (the separate
deferred `ADV_LOG_LEVEL` item) is banked.
