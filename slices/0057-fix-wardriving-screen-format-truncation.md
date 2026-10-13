---
id: '0057'
title: "Fix the wardriving-screen format-truncation warnings"
type: slice
status: accepted
date: 2026-10-13
supersedes: []
superseded_by: []
---

## Goal

Close the ledger's `[bug] wardriving_screen.cpp has two -Wformat-truncation warnings` (found
2026-10-13 during slice-0056's cardputer build):

- **`formatDuration` (line 316)** — a real latent overflow. `hours = seconds / 3600` is up to 7
  digits for a `uint32_t`, but the result is formatted into a `char[12]`; a duration past ~99 h
  (or a corrupt `seconds`) silently truncates the `HH:MM:SS` string.
- **The clock line (line 593)** — `buffer[36]` is in fact large enough for any real input, so the
  warning is the compiler unable to bound `tm_year` and the `const char* srcTag`. Fixing the warning
  also surfaced a genuine latent defect alongside it: `gmtime(&now)` is dereferenced unchecked
  (line 581/594), a NULL-deref if the epoch is unparseable.

`wardriving_screen.cpp` is excluded from the native build (`-<ui/>`), so a regression test needs the
pure logic to live in a natively-compiled unit. This slice therefore extracts the duration
formatter into a tested util and fixes the clock line in place.

## Definition of Done

- **Given** a duration whose hours need more than two digits (e.g. 123456 h)
- **When** it is formatted for display
- **Then** the full `H…H:MM:SS` string is produced with no truncation, and this is asserted by a
  native unit test that fails against the old `char[12]` sizing

- **Given** the clock line is drawn with a synced clock
- **When** the timestamp string is built
- **Then** it renders `YYYY-MM-DD HH:MM:SS [TAG]` within its buffer with no `-Wformat-truncation`
  warning, the field widths being provably bounded

- **Given** an epoch that `gmtime_r` cannot convert
- **When** the clock line would be drawn
- **Then** the function returns without dereferencing a NULL `tm` (the latent NULL-deref is closed)

- **Given** all three builds
- **When** they compile `wardriving_screen.cpp`
- **Then** neither line 316 nor line 593 emits a `-Wformat-truncation` warning

## Design

### D1 — Extract `formatDuration` into a tested util

`WardrivingScreen::formatDuration` uses no member state — it is a pure `uint32_t → "HH:MM:SS"`
function. Move that logic to a header-only `adversary::utils::formatDuration(uint32_t totalSeconds,
char* out, size_t outLen)` in `src/utils/time_format.h` (utils/ is in the native `build_src_filter`,
so a new `test_time_format` suite can exercise it). Hours keep their `%02lu` minimum-width padding
(so short durations still read `01:02:03`) but are allowed to grow; the caller sizes the buffer for
the `uint32_t` worst case (`1193046:59:59`, 14 bytes) at 16. The screen's `formatDuration` becomes a
thin `String` wrapper over the util, so the two existing call sites (lines 438, 477) are unchanged.

### D2 — Bound the clock line in place

The clock line stays in the (device-only, canvas-coupled) draw function; it is not extracted. The
`int` date fields are emitted through `%04u`/`%02u` of values reduced modulo their width — the same
idiom as slice-0056's `formatSessionId`, a no-op on valid data that hands the compiler the width
proof — and `srcTag` is bounded with `%.3s` (the reliability tags are fixed 3-char codes:
`NTP`/`GPS`/`~SD`). `gmtime` becomes `gmtime_r` into a local `struct tm` with a NULL-return guard,
closing the unchecked-deref. Worst-case output is then 25 bytes into the 36-byte buffer.

### D3 — Considered and not reused

- **Extract the clock line too** — rejected for now (the user's scope choice). It is coupled to the
  canvas and `TimeManager` source enum; a pure extraction buys little over the in-place bound, and
  its format already overlaps `formatWigleTimestamp`/`formatSessionId`. A future rule-of-three
  consolidation of the three gmtime formatters is noted but not forced here (global §3).
- **Just enlarge `char[12]`** — insufficient on its own: it fixes the symptom without a test, and
  the logic is not natively reachable where it lives. Extraction is what makes the fix testable.
- **`#pragma GCC diagnostic ignored`** — rejected (same reason as slice-0056): silences, does not
  prove.

## Verification

- **`pio test -e native`** gains a `test_time_format` suite:
  - `formatDuration(3661, …)` ⇒ `"01:01:01"` (normal, padded).
  - `formatDuration(444441600, …)` (123456 h) ⇒ `"123456:00:00"` — a 12-char result the old
    `char[12]` buffer could not hold, so this **fails before** the fix and **passes after**
    (DoD scenario 1).
- **`pio run -e cardputer`** and **`pio run -e m5stick`** compile with no `-Wformat-truncation` on
  `wardriving_screen.cpp` lines 316/593 (DoD scenarios 2, 4) — confirmed by grepping the full build
  logs. The NULL-guard (DoD scenario 3) is verified by read-through; it is in a device-only
  canvas-draw path with no native seam, consistent with the scope choice.

## As built

Shipped as designed. The duration formatter moved to header-only
`adversary::utils::formatDuration(totalSeconds, out, outLen)` in `src/utils/time_format.h`
(two-digit minimum width, hours uncapped); `WardrivingScreen::formatDuration` is now a thin `String`
wrapper over it with a 16-byte buffer, so its two call sites (lines 438, 477) are untouched. The
clock line stayed in place: `gmtime` → `gmtime_r` into a local `struct tm` with a NULL-return guard
(closing the unchecked deref), the date fields emitted through `%04u`/`%02u` of modulo-reduced
values, and `srcTag` bounded with `%.3s`.

**Verification run (2026-10-13):**

- `pio test -e native` — **822/822** (was 818; +4). New `test_time_format` suite: padded
  (`3661 → "01:01:01"`), zero, the overflow regression (`444441600 → "123456:00:00"`, a 12-char
  result the old `char[12]` could not hold — fails pre-fix), and the `uint32_t` max
  (`4294967295 → "1193046:28:15"`). DoD scenario 1.
- `pio run -e cardputer` SUCCESS, `pio run -e m5stick` SUCCESS. Grepping both full build logs:
  `wardriving_screen.cpp` lines 316 and 593 no longer warn (DoD scenarios 2, 4). The NULL-guard
  (scenario 3) is verified by read-through — the draw path has no native seam, per the scope choice.
- `lint-docs` / `check-immutable` green; `adr/INDEX.md` regenerated (55 decisions, only the 0057
  row added).

**Noted, not fixed (boyscout → ledger):** grepping the cardputer log for the whole warning class
turned up four *more* pre-existing `-Wformat-truncation` sites in unrelated screens —
`captures_screen.cpp:890`, `saved_networks_screen.cpp:93`, `rfid_screen.cpp:1380`,
`whitelist_screen.cpp:250`. They are out of this slice's scope (its bug was the two wardriving-screen
lines) and are logged as a single UI-wide follow-up `[bug]`.
