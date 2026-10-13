---
id: '0056'
title: "Fix the wardriving session-id buffer overflow"
type: slice
status: accepted
date: 2026-10-13
supersedes: []
superseded_by: []
---

## Goal

Close the ledger's `[bug] WardrivingManager::generateSessionId can overflow its 20-byte buffer`
(found 2026-10-12 during slice-0055). `generateSessionId` formats `"WD-%04d%02d%02d-%02d%02d%02d"`
into `session_.sessionId` (`char[20]`). The valid output — `WD-YYYYMMDD-HHMMSS` — is 18 chars + NUL
= 19, which fits. But the sanity gate (`now > 1000000000`) only bounds the clock from *below*: a
corrupt or far-future `now` yields `tm_year + 1900 ≥ 10000`, a 5+-digit year that `snprintf`
silently truncates into a malformed id. The native build flags this as `-Wformat-truncation`
("output between 19 and 71 bytes into a destination of size 20"), because the compiler cannot prove
the `int` fields fit. The id then drives the CSV/JSON filenames (`csvPath_`, `jsonPath`), so a
garbled id means a garbled filename.

This slice fixes the overflow at the root (validate the clock, bound the fields) rather than
silencing the warning, and extracts the logic into a pure, unit-testable helper so the fallback is
covered by a regression test.

## Definition of Done

- **Given** a synced clock whose epoch maps to a normal 4-digit year
- **When** the session id is generated
- **Then** it is the real `WD-YYYYMMDD-HHMMSS` UTC stamp and fits the buffer with no truncation

- **Given** a clock reading that maps to a year outside 2001–9999 (corrupt/unsynced/far-future)
- **When** the session id is generated
- **Then** it falls back to the uptime-hex form `WD-XXXXXXXX` — a mangled or truncated datestamp is
  never written (fail-safe, not fail-garbled)

- **Given** the native build
- **When** it compiles `wardriving_manager.cpp`
- **Then** there is no `-Wformat-truncation` warning on `generateSessionId`, because the formatted
  fields are provably within the buffer

## Design

### D1 — A pure helper, like slice-0055's timestamp fix

Extract `static WardrivingManager::formatSessionId(time_t now, uint32_t uptimeMs, char* out,
size_t outLen)` — no `millis()`/`TimeManager` reads inside, so the `test_wardriving` suite can drive
it directly. `generateSessionId` becomes the thin impure shell that supplies
`TimeManager::getInstance().now()`, `millis()` and `sizeof(session_.sessionId)` (the literal `20`
inside the function is deleted — the size comes from the real buffer at the call site, no magic
value). This mirrors `formatWigleTimestamp` added in slice-0055 and reuses its file-scope
`MIN_SANE_EPOCH` constant for the lower bound.

### D2 — Validate, don't suppress

The datestamp branch is taken only when `now >= MIN_SANE_EPOCH`, `gmtime_r` succeeds, **and** the
resulting year is in `[2001, 9999]`. The upper bound is the fix's substance: it routes an
out-of-range clock to the existing uptime-hex fallback instead of into a truncating `snprintf`.
Within that branch the fields are emitted through `%02u`/`%04u` of values reduced modulo their
width (`year % 10000`, the rest `% 100`) — on valid data (guaranteed by the range check and
`gmtime_r`'s own field ranges) these are no-ops, but they encode the bound the compiler needs so
`-Wformat-truncation` is satisfied by proof, not by a pragma. Worst-case output is then
`3 + 4 + 2 + 2 + 1 + 2 + 2 + 2 + NUL = 19 ≤ 20`.

UTC (`gmtime_r`) replaces the original `localtime`: the id is an opaque filename label, the device's
clock is GPS-sourced (already UTC) with no timezone configured (so `localtime` was UTC anyway), and
UTC makes the helper deterministic to test. This is behaviour-neutral on device and consistent with
the CSV `FirstSeen` field (also UTC since slice-0055).

### D3 — Considered and not reused

- **Just enlarge the buffer to the 71-byte worst case** — rejected. It masks the real defect (a
  garbage year still produces a garbage id) and bloats `WardrivingSession`; the valid id is 19
  bytes and should stay that size.
- **A `#pragma GCC diagnostic ignored`** — rejected. It hides the warning without fixing the
  truncation; fail-loud (global §3) wants the bound real, not silenced.
- **Reuse `formatWigleTimestamp`** — not applicable: that emits `YYYY-MM-DD HH:MM:SS`, a different
  shape; only the `MIN_SANE_EPOCH` constant is shared.

## Verification

- **`pio test -e native`** gains two `test_wardriving` cases calling
  `WardrivingManager::formatSessionId` directly:
  - *synced*: `now=1700000000` (2023-11-14 22:13:20 UTC) ⇒ asserts `"WD-20231114-221320"`.
  - *insane year*: `now=260000000000` (year > 9999) with `uptimeMs=43981000` ⇒ asserts the fallback
    `"WD-0000ABCD"`. This **fails before** the fix (the old code would format a 5-digit year and
    truncate) and **passes after** (DoD scenarios 1 and 2).
- **`pio run -e cardputer`** compiles with no `-Wformat-truncation` on `generateSessionId`
  (DoD scenario 3); **`pio run -e m5stick`** green. The verbose build is grepped to confirm the
  warning is gone, not just scrolled past.

## As built

Shipped as designed. `generateSessionId` is now a thin shell calling the new pure
`static WardrivingManager::formatSessionId(now, uptimeMs, out, outLen)`, passing
`TimeManager::getInstance().now()`, `millis()` and `sizeof(session_.sessionId)` — the literal
`20` inside the function is gone. `formatSessionId` takes the `WD-YYYYMMDD-HHMMSS` (UTC, `gmtime_r`)
branch only when `now >= MIN_SANE_EPOCH` **and** `gmtime_r` succeeds **and** the year is
`<= MAX_SESSION_ID_YEAR` (9999, named `constexpr`); otherwise it falls back to the uptime-hex
`WD-%08lX`. The emitted fields are reduced `% 10000`/`% 100` so the compiler can prove the width —
no `#pragma`. `MIN_SANE_EPOCH` is reused from slice-0055.

**Verification run (2026-10-13):**

- `pio test -e native` — **818/818** (was 816; +2). The two added `test_wardriving` cases call
  `formatSessionId` directly: *synced* (`now=1700000000`) asserts `"WD-20231114-221320"`;
  *insane year* (`now=260000000000` → year 10209, `uptimeMs=43981000`) asserts the fallback
  `"WD-0000ABCD"` (fails pre-fix — the old code snprintf'd a 5-digit year into the 20-byte buffer
  and truncated). DoD scenarios 1 and 2.
- `pio run -e cardputer` — SUCCESS (1:22); `pio run -e m5stick` — SUCCESS (1:12). Grepping both
  full build logs: `wardriving_manager.cpp` compiles with **zero** warnings — the
  `-Wformat-truncation` on `generateSessionId` is gone (DoD scenario 3).
- `lint-docs` / `check-immutable` green; `adr/INDEX.md` regenerated (54 decisions, only the 0056
  row added).

**Noted, not fixed (boyscout → ledger):** the same cardputer build surfaced two *pre-existing*
`-Wformat-truncation` warnings in `wardriving_screen.cpp` (lines 316 and 593) — a different,
UI-layer file, so out of this branch's scope. Logged as its own `[bug]`.
