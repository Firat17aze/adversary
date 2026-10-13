---
id: '0059'
title: "Move RFID from modules/ into hal/ to fix the layering"
type: slice
status: accepted
date: 2026-10-13
supersedes: []
superseded_by: []
---

## Goal

Pick up the RFID half of the ledger's `HardwareManager` architecture item:

> `HardwareManager` to stop HAL leakage … includes moving RFID into `src/hal/` (still in
> `src/modules/`).

RFID is a hardware driver (MFRC522 over SPI/I2C), not a feature module — it belongs in the
Hardware Abstraction Layer next to the other device drivers, not under `src/modules/` beside
scanner/attack/wardriving business logic. This slice performs **only** that relocation:
`src/modules/rfid/` → `src/hal/rfid/`, a pure layering fix with **no behaviour change**.

The item's other half — a board-capabilities singleton (`HardwareManager` with `hasKeyboard()`,
`getBatteryVoltage()`) folding the `#if defined(TARGET_*)` conditionals — is **deliberately not
done here**. It reverses the repeated speculative-generality choice recorded in slices 0002/0003/0004
(see `modules/ir/ir_capability.h`, which names the "(non-existent) `HardwareManager`"), it would
introduce the category-(b) singleton ADR-0058 tells new code to avoid, and its only consumer —
non-M5 board portability — has zero demand yet. That half stays folded into the portability ledger
item (the `BoardProfile` seam), where it is the opening step of a future multi-slice migration. This
slice closes the RFID sub-task so the bundled ledger line can be narrowed to just the capabilities
half.

## Definition of Done

- **Given** the native test build after the move
- **When** `pio test -e native` runs
- **Then** the four RFID suites (`test_rfid_audit`, `test_rfid_dump`, `test_rfid_manager`,
  `test_rfid_writer`) still compile and run — proving RFID was re-added to the allowlist *after* the
  `-<hal/>` exclusion and did not silently drop out of the build

- **Given** both device targets
- **When** `pio run -e cardputer` and `pio run -e m5stick` build
- **Then** both link successfully with every `#include` of the RFID headers resolved from the new
  `hal/rfid/` path

- **Given** the relocation
- **When** the firmware runs
- **Then** behaviour is unchanged — this is a move plus include-path and build-filter edits only; no
  RFID source line is modified

## Design

### D1 — The move

`git mv` the ten files of `src/modules/rfid/` (`rfid_audit`, `rfid_dump`, `rfid_manager`,
`rfid_reselect`, `rfid_writer` — `.h`/`.cpp` each) to `src/hal/rfid/`. The source and destination are
both two levels under `src/`, and the files use no `../`-relative includes internally, so **no edit
inside the moved files is needed** — their outbound includes (`config/pins.h`, etc.) are unaffected.

### D2 — Inbound include rewrites (mechanical, 11 lines / 7 files)

Every `#include` that names the old path flips `modules/rfid/` → `hal/rfid/`:

| File | Lines |
|---|---|
| `src/main.cpp` | 1 (`modules/rfid/rfid_manager.h`) |
| `src/ui/components/status_bar.h` | 1 (`../../modules/rfid/` → `../../hal/rfid/`, same depth) |
| `src/ui/screens/rfid_screen.h` | 4 (manager/audit/dump/writer) |
| `test/test_rfid_audit`, `…_dump`, `…_manager`, `…_writer` | 5 |

### D3 — The build-filter edit (the one non-mechanical change)

`platformio.ini` `[env:native]` `build_src_filter` is an ordered allowlist where **later rules
win**. `+<modules/rfid/>` currently sits *before* `-<hal/>`. Renaming it in place to `+<hal/rfid/>`
would leave it *before* the `-<hal/>` exclusion, so RFID would be excluded from the native build and
the four RFID suites would silently stop compiling — a hidden coverage loss. The edit therefore
**removes** `+<modules/rfid/>` from its current position and **adds** `+<hal/rfid/>` *after* the
`-<hal/>` line, exactly as `+<hal/storage/sd_manager.cpp>` is already re-added there. DoD scenario 1
is the guard that this was done correctly.

### D4 — Considered and not reused

- **Build the `HardwareManager` capabilities singleton now** — rejected. No non-M5 demand; it
  reverses the 0002/0003/0004 speculative-generality precedent and introduces a category-(b)
  singleton ADR-0058 discourages. It belongs to the portability item, not here.
- **Promote this to a `good first issue`** — rejected per ADR-0022 criterion (a): the build-filter
  ordering trap (D3) is not legible without deep context. An outsider would move the files, see the
  device builds pass, and never notice the native suites vanished. The failure mode is a silent
  coverage loss, so this is a keep-and-do, not a promote.
- **Leave RFID in `modules/`** — rejected: it is the HAL leakage the ledger item names; RFID is a
  driver, and the move is cheap and behaviour-neutral.

## Verification

This slice adds **no logic**, so it introduces no new unit test — a relocation has nothing new to
assert. Its regression guard is the *existing* test and build surface, which is exactly what the D3
trap would break:

- **`pio test -e native`** — the four `test_rfid_*` suites must still be collected and pass. Their
  continued presence is the proof that `+<hal/rfid/>` was re-added after `-<hal/>` (DoD scenario 1).
  Confirmed by the suite count being unchanged from the pre-move run and the four names appearing in
  the output.
- **`pio run -e cardputer`** and **`pio run -e m5stick`** — both link, proving all seven inbound
  include sites resolve from the new path (DoD scenario 2).
- **No behaviour change** (DoD scenario 3) is verified by `git diff -M` showing the ten files as pure
  renames (100% similarity) with zero content delta, and the only non-rename edits being include
  paths and the one `build_src_filter` line.

## As built

Shipped as designed — a pure relocation with no behaviour change. The ten `src/modules/rfid/`
files moved to `src/hal/rfid/` via `git mv` (confirmed `rename … (100%)`, 0 insertions / 0
deletions inside them); the only content edits are the eleven inbound `#include` lines across the
seven dependent files (`main.cpp`, `status_bar.h`, `rfid_screen.h`, and the four `test_rfid_*`
suites), each `modules/rfid/` → `hal/rfid/`. In `platformio.ini` the native allowlist entry moved
from `+<modules/rfid/>` (which sat before `-<hal/>`) to `+<hal/rfid/>` placed *after* `-<hal/>`,
beside `sd_manager.cpp`, so the four RFID suites keep compiling natively (D3).

**Verification run (2026-10-13):**

- `pio test -e native` — **822/822** (unchanged from the pre-move baseline). The four RFID suites
  (`test_rfid_audit`, `test_rfid_dump`, `test_rfid_manager`, `test_rfid_writer`) are still collected
  and pass — the proof that `+<hal/rfid/>` was re-added after `-<hal/>` and RFID did not drop from
  the build (DoD scenario 1).
- `pio run -e cardputer` SUCCESS (RAM 26.5%, Flash 74.5%) and `pio run -e m5stick` SUCCESS (RAM
  21.0%, Flash 77.4%) — both link with every RFID include resolved from the new path (DoD
  scenario 2).
- `git diff -M` shows the ten files as 100%-similarity renames with zero content delta; the only
  other hunks are the include-path lines (DoD scenario 3).

**Toolchain note (not a code issue):** running the native test and both device builds concurrently
raced PlatformIO's shared package installer and triggered a `framework-arduinoespressif32` reinstall,
which left stale `.pio/` build metadata — the device builds failed on objcopy/include-path resolution
until `pio run -t clean` regenerated the build config. No `~/.platformio` surgery was needed and no
source was involved; the native result (host toolchain) was never affected. Lesson recorded: build
the three environments serially, not in parallel.

**Scope note:** this closed only the RFID sub-task of the bundled `HardwareManager` ledger item. The
board-capabilities-singleton half remains deferred and is now folded into the non-M5 portability
item (the `BoardProfile` seam), per the Design above and ADR-0058.
