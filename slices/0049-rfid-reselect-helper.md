---
id: '0049'
title: "One robust re-select helper for every full-select RFID site"
type: slice
status: accepted
date: 2026-10-11
supersedes: []
superseded_by: []
---

## Goal

Collapse the four "wake the tag into a known ACTIVE state" sites onto one helper, so the
HALT-before-WUPA lesson from slice-0046 cannot hold in one site while silently regressing in
another. Today the four sites have drifted apart in robustness, and one of them (the audit's
per-sector re-select) still does the exact bare-WUPA that slice-0046 proved fails on hardware.

## Background — the four full-select sites (pre-slice)

A "full select" is: the operator needs the tag freshly woken and selected before an auth. On a
WS1850S/clone an **ACTIVE tag ignores WUPA (0x52) until it is HALTed** (field-observed on
`23FF9C1E`, slice-0046), so a correct re-select must `PCD_StopCrypto1 → PICC_HaltA → WUPA
(→ REQA fallback) → PICC_ReadCardSerial`, and the backdoor/cold-field timing is flaky enough to
want a few retries (slice-0044).

| Site | File / anchor | HALT first? | Retry? | REQA fallback? |
|------|---------------|:-----------:|:------:|:--------------:|
| Writer `selectCard` | `rfid_writer.cpp` | yes | 5× | yes |
| Audit initial select | `rfid_audit.cpp` (pre-sector loop) | yes | 5× | yes |
| Audit read re-select | `rfid_audit.cpp` (dump read path) | yes | no | no |
| Audit per-sector re-select | `rfid_audit.cpp` (sector-start) | **no** | no | no |

The writer and the audit-initial site are already the robust form (and are byte-for-byte the same
loop — pure duplication). The read re-select got its HALT in slice-0046 but never got retry. The
per-sector re-select is the laggard: a bare `WUPA → ReadCardSerial` on a tag the previous sector's
auth left ACTIVE — the precise failure mode slice-0046 fixed elsewhere. It has worked in the field
only because `tryKey`'s own per-failure WUPA happens to recover on the first key of the next sector;
that is luck, not design.

(The lightweight per-failure recoveries **inside** `tryKey` and `RfidWriter::tryAuth` are *not* full
selects — they run hundreds of times per audit, and a 5-retry/escalating-delay helper there would
balloon audit time. They stay as they are. This slice is only the four full-select sites.)

## Design

### D1 — One free helper, shared

`bool adversary::reselectCard(MFRC522_I2C*)` in new `src/modules/rfid/rfid_reselect.{h,cpp}`, the
writer's proven loop verbatim, with its magic numbers named (`RESELECT_ATTEMPTS = 5`,
`RESELECT_HALT_SETTLE_MS = 20`, `RESELECT_RETRY_BASE_MS = 30`). A free function (not a method) so both
`RfidWriter` and `RfidAudit` call the one implementation; `RfidWriter::selectCard()` becomes a thin
delegator so its existing ~5 call sites are untouched. `+<modules/rfid/>` already globs the whole
directory into both the `native` and `cardputer` builds, so the new TU needs no `platformio.ini`
change.

### D2 — Behaviour change is on-hardware only, so the mock gains a faithful seam

As the mock stood, `PICC_WakeupA` always returned `STATUS_OK` regardless of tag state, so it could
**not** tell a bare WUPA from a HALT-first one — routing the per-sector site through the helper was
native-behaviour-neutral, and the native suite could not guard the very lesson this slice exists to
protect. So this slice adds a gated seam `g_mockWupaNeedsHalt` (default **off**, so every other test
is untouched) that models the real quirk with a three-state tag (`ACTIVE`/`HALTED`/`READY`):

- a successful `PCD_Authenticate` and a completed select leave the tag `ACTIVE`;
- under the seam, `WUPA`/`REQA` on an `ACTIVE` tag time out (the tag ignores them) and only wake a
  `HALTED` tag; `ReadCardSerial` selects only a `READY` (woken) tag;
- so a re-select that skips `PICC_HaltA` never wakes an `ACTIVE` tag, leaving it unselected.

This composes with the existing `g_mockReauthNeedsReselect` desync (slice-0046): an unwoken tag means
`g_mockSelectedSinceAuth` stays false, which turns the *next* auth into a failure — the observable
symptom. No parallel auth-failure path is invented.

### D3 — The regression

`test_audit_reselect_requires_halt_between_sectors` (in `test_rfid_audit`): with
`g_mockWupaNeedsHalt` **and** `g_mockReauthNeedsReselect` on, run a full audit of an all-factory-key
card. **Fails-before:** the bare per-sector WUPA never wakes the ACTIVE tag, so sectors 1–15 do not
authenticate and only sector 0's keys are found. **Passes-after:** every sector re-selects through
the HALT-first helper and all sector keys are found. The two already-HALT-first sites (writer,
audit-initial) and the read re-select are behaviour-neutral under the seam (HALT→HALT); the suite
proves they did not regress.

## Definition of Done

- **Given** a card left ACTIVE (as the previous sector's auth leaves it) and the per-sector re-select
  running at the start of sectors 1–15
- **When** an all-factory-key card is audited
- **Then** every sector's re-select halts before WUPA, so every sector authenticates and Key A is
  cracked in all 16 sectors (not just sector 0)

- **Given** the writer's `selectCard` and the audit's initial and read-path selects (already HALT-first)
- **When** they are routed through the one `reselectCard` helper
- **Then** behaviour is unchanged (the full native suite stays green) and no bare-WUPA full-select
  remains anywhere in the module

- **Given** the extracted helper is one copy of a loop that was duplicated four ways
- **When** the Cardputer firmware is built
- **Then** it links with Flash no larger than before (a pure refactor)

## Verification

- **Native (`test_rfid_audit`):**
  - `test_audit_reselect_halts_before_wupa_between_sectors` — `g_mockWupaNeedsHalt = true` (an ACTIVE
    tag ignores a bare WUPA) composed with `g_mockReauthNeedsReselect = true` (the slice-0046 desync).
    Asserts an all-factory card cracks Key A in all 16 sectors (`keysFound == 16`). **Fails before:**
    the per-sector re-select is a bare WUPA that never wakes the ACTIVE tag, so only sector 0 cracks
    (`Expected 16 Was 1`). Key B stays unrecovered under the seam by design (it leans on `tryKey`'s
    un-helpered lightweight recovery), so the count is 16, not the clean-mock 32.
  - The other audit tests run with the seam off (default), proving it is gated and the already-HALT
    sites did not regress.
- **Cardputer build:** `pio run -e cardputer` green, RAM/Flash recorded in As built.
- **On-device (on the confirmed Gen1a `23FF9C1E`, non-destructive):**
  1. Full audit — every sector reads back, **0 `??`** (no regression vs slice-0046's clean read).
  2. A write-back/clone still lands (the writer's `selectCard` path through the helper is unchanged).

## As built

`src/modules/rfid/rfid_reselect.{h,cpp}` holds the one `reselectCard(MFRC522_I2C*)`; the writer's
`selectCard()` is now a one-line delegator, and the three audit sites (initial, per-sector, read) call
it directly. The two bare-WUPA/single-shot forms are gone. The mock gained `g_mockWupaNeedsHalt`
(default off) with a three-state tag, composing with `g_mockReauthNeedsReselect`.

- Native: full suite **808/808** (was 807; +`test_audit_reselect_halts_before_wupa_between_sectors`).
  Regression proven fails-before (`Expected 16 Was 1`, per-sector bare WUPA never wakes the ACTIVE
  tag so only sector 0 cracks) / passes-after (16). The other audit tests pass with the seam off,
  confirming it is gated.
- `pio run -e cardputer` **SUCCESS** — RAM 26.5% (86704 B, unchanged), Flash 74.3% (2483835 B, **200 B
  smaller** than v0.3.0-alpha's 2484035 B: the duplicated 5-retry select loop collapsed to one copy).

**On-device PENDING** on `23FF9C1E`: a full audit still reads every sector (0 `??` regressions) and a
write-back/clone still lands, confirming the unified helper did not change field behaviour.

## Amendment — 2026-10-11: on-device confirmed

Flashed `fix/rfid-postrelease-cleanups` to the Cardputer and ran the verify on `23FF9C1E`:

- **Audit:** Key A cracked in all 16 sectors; the dump saved to `/adversary/captures/RFID/23FF9C1E.nfc`
  with **zero** `??` / `READ FAILED` / `AUTH FAILED` / `omitted` markers in the serial log — no
  regression to slice-0046's clean per-sector reads, which is the site this slice actually changed.
- **Write-back (clone):** `Write done: 47/47 data blocks, 0 auth-fail, 0 write-fail. UID written -
  Gen1a backdoor magic confirmed`; a re-detect then read `23FF9C1E ... MAGIC: YES`. The writer's
  `selectCard` path through the shared helper is unchanged.

The one run also confirmed the two sibling cleanups committed alongside this slice: the write summary
now prints `47/47 data blocks` (the former `47/48` off-by-one is gone), and deleting the redundant
REQA keep-alive left key discovery intact (all 16 sectors still crack). All DoD items met.
