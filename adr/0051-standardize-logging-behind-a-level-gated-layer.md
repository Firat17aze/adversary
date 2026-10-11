---
id: '0051'
title: "Standardize logging behind a level-gated layer"
type: architecture
status: accepted
date: 2026-10-11
supersedes: []
superseded_by: []
---

# ADR-0051 — Standardize logging behind a level-gated layer

## Context

The firmware has four coexisting, unrelated logging idioms, none of them level-gated —
every one ships its output unconditionally in the release image. Measured on `main` at
this ADR's date (`src/`, probe in `scripts/` history):

- **Raw `Serial.print` / `println` / `printf` — 583 call sites across 62 files.** The
  dominant idiom. Their string literals alone are **~24.0 KB of rodata** (floor: dedup and
  alignment ignored, a lower bound on what the literals occupy), carried in every build.
- **`adversary::logger` (`src/utils/error_logger.h`) — 3 call sites.** Appends
  `[ERROR]`/`[WARN]` records to `/adversary/logs/error.log` on SD *and* mirrors to Serial.
  The only idiom that persists anything, yet it is used in 3 places.
- **Ad-hoc `LOG_I`/`LOG_W`/`LOG_E` `#define`s — 5 definitions duplicated across the 2
  wardriving files** (`wardriving_manager.cpp`, `wardriving_exporter.cpp`), each expanding
  to a `Serial.printf("[LEVEL] " fmt "\n", …)`.
- **Dead scaffolding in `src/config/config.h` — `DEBUG_ENABLED`, `DEBUG_LOG`, `LOG_TAG`
  "ADV", with 0 uses each.** `CORE_DEBUG_LEVEL=3` (platformio.ini) feeds only the unused
  `DEBUG_ENABLED`, so the one knob that looks like a log-level control gates nothing.

The forces that make this worth fixing now, not an aesthetic cleanup:

- **Flash pressure is real and measured.** Cardputer is at **74.3 %** flash
  (2 483 527 B); the m5stick target sits near its OTA-less `huge_app.csv` ceiling (tracked
  flash-ceiling item). The ~24 KB of unconditional Serial-literal rodata is a concrete
  slice of that budget, much of it info/debug chatter that a release build has no use for.
- **No way to quiet the firmware.** Verbose per-packet / per-sector chatter cannot be
  silenced for a field build without hand-deleting lines, because nothing is gated.
- **Four idioms to learn and maintain**, and errors are persisted only by accident of which
  of the four a given site happened to use.

## Decision

Introduce **one thin project logging layer**, `src/utils/log.h`, and route all logging
through it. The layer is a set of macros — `LOG_E(tag, fmt, …)`, `LOG_W(…)`, `LOG_I(…)`,
`LOG_D(…)` — with these fixed properties:

1. **Compile-time level gating by a dedicated project flag** (e.g. `ADV_LOG_LEVEL`), *not*
   `CORE_DEBUG_LEVEL`. A project flag decouples us from ESP-IDF's `ESP_LOGx` semantics and
   works identically on the native test target; `CORE_DEBUG_LEVEL` is left to mean only what
   ESP-IDF uses it for. A level below a macro's level compiles that macro to nothing, so its
   format string is **not emitted to rodata** — this is the mechanism that recovers flash.
2. **Errors and warnings are always compiled in; info and debug are the only strippable
   levels.** Release builds strip info/debug; debug builds keep them. This preserves
   fail-loud (global quality bar): an error is never silently gated away.
3. **On the native target (`UNIT_TEST`) the macros map to `printf`**, so tests keep building
   and their diagnostics keep printing — matching how `error_logger.h` already shims native.
4. **Errors may tee to the existing SD `ErrorLogger`.** The layer *reuses*
   `adversary::logger`, it does not replace it: `LOG_E` can both print and append to
   `/adversary/logs/error.log`, so error persistence becomes the default instead of a
   3-site accident. `error_logger.h` stays the persistence sink; `log.h` is the front door.
5. **The dead `config.h` scaffolding and the 5 ad-hoc wardriving macros are retired** and
   folded into the layer. `LOG_TAG "ADV"` is replaced by the per-call `tag` argument, which
   is how a module names its own log lines.

**Migration is convention + lint, then incremental — not a big-bang rewrite:**

- A new CI lint (modeled on the existing `scripts/lint-serial-escapes.mjs` + its CI
  `serial-escapes` job) **forbids *new* raw `Serial.print*` outside the layer**. Legacy
  sites are tolerated until migrated, so the lint can land before the migration finishes.
- The 583 existing sites are migrated **module by module in follow-up slices**, each slice
  converting one cohesive area, keeping its own tests green, and small enough to review.
- The **precise release flash delta** (info/debug stripped vs. current) is **measured by the
  first implementing slice's probe** — it needs the layer and one converted module in place
  to measure honestly. The ~24 KB rodata floor above is the ceiling on literal-string
  savings, not the predicted delta; the slice reports the real number, per "measure, don't
  estimate."

## Consequences

- **One idiom.** Every new log line is `LOG_x(tag, …)`; reviewers and the lint have a single
  shape to check. The four-idiom sprawl converges as modules migrate.
- **Release flash becomes recoverable** by stripping info/debug — the central justification.
  The actual recovered bytes are reported by the first migration slice, not promised here.
- **Error persistence becomes the default**, not a 3-site accident, because `LOG_E` tees to
  the SD `ErrorLogger`.
- **Native tests keep building** — the macros are `printf` under `UNIT_TEST`.
- **An interim period has mixed styles.** The lint blocks only *new* raw `Serial.print*`; the
  legacy 583 coexist with the layer until their module's migration slice lands. This is the
  deliberate cost of not doing a big-bang rewrite, and it is bounded by the open ledger item.
- **Fail-loud is protected**, not weakened: errors/warnings are never strippable, so gating
  can never hide a broken invariant — only debug/info chatter is removed from release.
- The **module-by-module migration remains an open `LEDGER.md` item** citing this ADR; this
  ADR decides the mechanism, it does not perform the migration.
- Any future change to the level semantics (what ships in release vs. debug, the flag name)
  requires a new ADR superseding this one.
