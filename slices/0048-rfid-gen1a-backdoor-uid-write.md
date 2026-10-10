---
id: '0048'
title: "Gen1a backdoor UID-write to complete the card clone"
type: slice
status: accepted
date: 2026-10-11
supersedes: []
superseded_by: []
---

## Goal

A full clone — UID included — onto a **Gen1a** magic card. Today the writer clones every data
block and writes the UID only on a Gen2/CUID direct-write card; on a Gen1a (block 0 writable *only*
through the backdoor) the UID never lands. Close that gap so "RFID Write / clone" clones the one
confirmed magic card we have (`23FF9C1E`), not just data blocks.

## Problem

`restore()`'s opt-in UID step ([rfid_writer.cpp](../src/modules/rfid/rfid_writer.cpp), the
`includeUid && plan.hasManufacturer` block) authenticates sector 0 and issues a plain
`MIFARE_Write(0)`. That write lands only on a **Gen2/CUID** card, whose block 0 answers a normal
authenticated write. A **Gen1a** card's block 0 is read-only to normal commands and is rewritable
*only* after the `MIFARE_OpenUidBackdoor` (0x40/0x43) sequence — the very sequence
`probeMagicGen1a()` ([rfid_manager.cpp:197](../src/modules/rfid/rfid_manager.cpp)) already ACKs for
*detection*. So the firmware can already tell a Gen1a card it is magic, then fails to write its UID.

Two further wrongnesses in the current block:
- The NAK verdict reads `"block 0 locked (ordinary / not Gen2-CUID)"` — but a Gen1a card is **not**
  ordinary; it is magic and writable, just not by this path. The operator is told the wrong thing.
- The backdoor write needs **no** authentication, yet the current code only reaches the write after
  a successful sector-0 auth. On a Gen1a whose sector-0 keys we do not hold, auth fails and the code
  gives up before it would even try the path that does not need auth.

The real library exposes every primitive needed — `MIFARE_OpenUidBackdoor(logErrors)`,
`MIFARE_Write(0, …)` (unauthenticated once the backdoor is open), and `MIFARE_UnbrickUidSector` as a
recovery net (confirmed in `.pio/libdeps/cardputer/MFRC522_I2C/src/MFRC522_I2C.h`).

## Scope

**In:** a two-strategy UID write that self-classifies by fall-through — attempt the existing
Gen2/CUID authenticated write first; if it does not land (auth fails *or* the write NAKs), re-select
to a clean ACTIVE state and attempt `MIFARE_OpenUidBackdoor` → unauthenticated `MIFARE_Write(0,
block0, 16)`. Report which strategy landed the UID (or that neither did, i.e. an ordinary card).
Write the **dump's own 16-byte block 0** (real UID + valid BCC). A mock seam modelling a Gen1a card
(plain block-0 write NAKs, backdoor-opened write succeeds), plus native regressions.

**Out:**
- Sector-trailer writes — still deferred (irreversible brick risk; a dump cannot carry the real key
  bytes anyway).
- `MIFARE_UnbrickUidSector` auto-recovery — we write a valid-BCC block, so the brick path is not
  exercised; a backdoor-write NAK is reported loud, not silently "unbricked". Logged separately if a
  real brick is ever observed.
- Passing the magic verdict from the detector into the writer — the fall-through self-classifies, so
  no new plumbing between `RFIDManager` and `RfidWriter`.

## Definition of Done

- **Given** a Gen1a card (sector-0 auth may or may not succeed; a plain block-0 write NAKs) and a
  dump whose block 0 carries the clone's UID
- **When** the operator restores with UID on
- **Then** the writer opens the backdoor and writes block 0, `uidWritten` is true, and the verdict
  names the **Gen1a backdoor** path

- **Given** a Gen2/CUID card (plain authenticated block-0 write succeeds)
- **When** the operator restores with UID on
- **Then** the write lands on the first (direct) strategy, the backdoor is never opened, and the
  verdict names **Gen2/CUID** — unchanged from today

- **Given** an ordinary card (plain write NAKs **and** the backdoor is not ACKed)
- **When** the operator restores with UID on
- **Then** both strategies fail loud, `uidWritten` is false, and the verdict says block 0 is locked
  on an ordinary (non-magic) card — the data writes still complete regardless

## Design

Restructure the UID step into two ordered strategies; `uidAttempted` stays true for the whole step.

1. **Direct (Gen2/CUID), as today.** `selectCard()` → `authBlock(dump, 0, 0)`; on auth success,
   `MIFARE_Write(0, block0, 16)`. On OK: `uidWritten = true`, verdict `"UID written - Gen2/CUID
   direct-write"`, `StopCrypto1`. An auth failure here is **no longer terminal** — it falls through
   to strategy 2 (the backdoor needs no auth), instead of returning the old "sector 0 auth failed".

2. **Backdoor (Gen1a), only if the UID has not already landed.** `StopCrypto1` + `selectCard()` for
   a clean ACTIVE state, then `MIFARE_OpenUidBackdoor(false)`:
   - returns true → `MIFARE_Write(0, block0, 16)`; on OK `uidWritten = true`, verdict `"UID written -
     Gen1a backdoor"`; on NAK, verdict `"UID not written - Gen1a backdoor opened but block 0 write
     NAK'd"` (fail loud — the card answered the backdoor but refused the write).
   - returns false → verdict `"UID not written - block 0 locked (ordinary card, not magic)"`.

Why fall-through rather than reading the detector's verdict: the write result *is* the ground truth
(the direct write proves Gen2/CUID; the backdoor ACK proves Gen1a), it needs no cross-module state,
and it degrades correctly if detection and write happen to disagree. Order is direct-first because
the direct write is the non-destructive, auth-gated path; the backdoor is only reached when the
ordinary path cannot land the UID.

**Reporting:** the three outcomes are distinguished by the `uidResult` verdict string (`"Gen2"` /
`"Gen1a"` / `"locked"` substrings), matching how the existing writer tests assert. No new
`WriteResult` field — the operator-facing verdict already carries the distinction, and string
verdicts are the established house pattern in this struct. The `restore()` summary line and the
`rfid_writer.h` doc comment for `includeUid` are updated to describe both strategies (the current
comment claims only the Gen2/CUID verdict).

**Mock seam:** a Gen1a card = a plain block-0 write NAKs, but a write issued *after* the backdoor is
open succeeds. Model it with a `g_mockBackdoorOpen` flag that `MIFARE_OpenUidBackdoor` sets on a true
verdict and a re-select clears; `MIFARE_Write` NAKs block 0 when `g_mockBlock0Nak && !g_mockBackdoorOpen`
and records+OK once the backdoor is open. Reset in `setUp()`. Off by default, so every existing test
is untouched. (`g_mockMagicGen1a` already drives the `OpenUidBackdoor` verdict.)

## Verification

- **Native (`test_rfid_writer`):**
  - `test_writer_clones_uid_via_gen1a_backdoor` — `g_mockBlock0Nak = true` (direct write NAKs) +
    `g_mockMagicGen1a = true` (backdoor ACKs). Asserts `uidWritten`, the verdict names the Gen1a
    backdoor, and a block-0 write is recorded in `g_mockWrites`. **Fails before:** no backdoor branch
    exists, so the direct NAK ends the step with `uidWritten == false` / "locked".
  - Extend `test_writer_uid_locked_reports_and_does_not_abort` — with `g_mockMagicGen1a = false` it
    now also covers the ordinary-card negative of the new branch (backdoor refused → "locked",
    `uidWritten == false`), data writes still complete.
  - `test_writer_uid_success_reports_gen2` — unchanged: direct write succeeds, backdoor never reached.
  - Full suite stays green; `pio run -e cardputer` builds (watch Flash — the backdoor primitives are
    already linked by the detector, so no material growth expected).
- **On-device (reversible, on the confirmed Gen1a `23FF9C1E`):**
  1. Audit + save `23FF9C1E` to its own `.nfc` (baseline, so the change is reversible).
  2. Restore a **distinct** block 0 (a second card's dump, or `23FF9C1E`'s dump with the UID edited)
     with UID on; confirm the serial log reads `UID written - Gen1a backdoor` and a fresh audit reads
     back the **new** UID.
  3. Restore `23FF9C1E`'s baseline dump to return the card to its original UID (Gen1a is always
     rewritable — this is why the on-device test is safe).

## As built

**Code (2026-10-11).** `restore()`'s UID step is now two ordered strategies over the dump's 16-byte
block 0 (`rfid_writer.cpp`). Strategy 1 is the former authenticated write (Gen2/CUID); a sector-0
auth failure is no longer terminal — it falls through. Strategy 2, reached only when the UID has not
yet landed, re-selects to a clean ACTIVE state, calls `MIFARE_OpenUidBackdoor(false)`, and on an ACK
issues the unauthenticated `MIFARE_Write(0, …)`. The three verdicts are distinguished by substring
(`Gen2/CUID` / `Gen1a` / `locked ... ordinary card, not magic`), and a backdoor ACK followed by a
write NAK is its own loud verdict. The `includeUid` doc comment (`rfid_writer.h`) now describes both
strategies. No new `WriteResult` field. `mfrc522_mocks.h` gains `g_mockBackdoorOpen`: set by a true
`MIFARE_OpenUidBackdoor`, cleared by `PICC_HaltA` (a re-select), and consulted by `MIFARE_Write` so a
block-0 write lands under `g_mockBlock0Nak` only while the backdoor is open. A NAK'd block-0 write is
now recorded nowhere (consistent with the slice-0047 transient model).

**Verified in-conversation (2026-10-11):**
- `pio test -e native -f test_rfid_writer` → 8/8; full suite `pio test -e native` → 806/806 (was 805,
  +1).
- Fails-before proof: `git stash push -- src/modules/rfid/rfid_writer.cpp` (the pre-slice one-strategy
  UID write) with the new mock + test makes `test_writer_clones_uid_via_gen1a_backdoor` fail —
  `Expected TRUE Was FALSE` on `uidWritten` (the direct NAK ends the step with no backdoor fallback).
  Restored; passes. The revised ordinary-card test (`test_writer_uid_locked_reports_and_does_not_abort`)
  passed under both the old and new writer, confirming the mock's "NAK records nothing" change is
  behaviour-neutral for that path.
- `pio run -e cardputer` → SUCCESS (RAM 26.5%, Flash 74.3% — unchanged; the backdoor primitives were
  already linked by `probeMagicGen1a()`, so no code growth).

**On-device round 1 (2026-10-11) — found a real miss, fixed:** the first flash wrote all 47 data
blocks but reported `UID not written - block 0 locked (ordinary card, not magic)` on `23FF9C1E`,
while the detector immediately after reported `MAGIC: YES`. Root cause: the UID step opened the
backdoor with a **single** `MIFARE_OpenUidBackdoor`, and the Gen1a backdoor wake is timing-sensitive
(slice-0044 — the same card reads NO then YES). Fix: retry the open `BACKDOOR_OPEN_ATTEMPTS = 5`
times with a `selectCard()` re-select between tries, mirroring `probeMagicGen1a()`. New regression
`test_writer_retries_flaky_gen1a_backdoor` (`g_mockMagicAckOnAttempt = 3`) fails-before
(`Expected TRUE Was FALSE`) and passes-after; suite 807/807; `pio run -e cardputer` SUCCESS (Flash
74.3%, +16 B).

**On-device round 2 (2026-10-11) — CONFIRMED, reversible full clone on `23FF9C1E`:**
- Restored a distinct dump (`C2D19C1E.nfc`) with UID on → `Write done: 47/48 ... UID written - Gen1a
  backdoor magic confirmed`; a fresh audit then read `Tag Detected: C2D19C1E (MAGIC: YES)` — the UID
  had actually changed to the clone.
- Restored `23FF9C1E`'s own baseline dump → `UID written - Gen1a backdoor magic confirmed`; audit read
  `Tag Detected: 23FF9C1E` again. The round-trip proves the backdoor UID-write lands and is
  reversible (Gen1a block 0 is always rewritable).
