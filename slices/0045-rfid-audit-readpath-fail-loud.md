---
id: '0045'
title: "Make the RFID audit read-path fail loud: attribute every unreadable block"
type: slice
status: accepted
date: 2026-10-10
supersedes: []
superseded_by: []
---

## Goal

Every `??` in a streamed `.nfc` dump, and every sector dropped from it, must be attributable
from the serial log to one of three causes — no dictionary key, an auth miss on read-back, or a
read miss after a good auth. A field run of the audit must be self-diagnosing instead of silently
producing gaps.

## Problem

`RfidAudit::runDictionaryAttack()`'s read path writes a `Block N: ?? ... ??` placeholder from two
different failure branches — auth failed, and read failed after a good auth — with no log to tell
them apart ([rfid_audit.cpp](../src/modules/rfid/rfid_audit.cpp), the per-block loop). Worse, a
sector for which no dictionary key is found is dropped from the dump entirely (the block/key write
is gated on a key being present), its only trace a terse `Sector N done: A=N B=N`.

On card `23FF9C1E` this produced a dump with 32 `??` data blocks and 7 sectors whose keys then
failed on write-back, and nothing in the evidence says which failure caused which gap. A silenced
failure is a bug debugged twice (CLAUDE.md rule 3). It also blocks the Step-2 decision: whether to
read magic cards through the Gen1a backdoor is a guess until we know the `??`s are key misses, not
read misses.

This slice adds the attribution. It does **not** change what the dump contains or how keys are
recovered — that is Step 2.

## Scope

**In:** distinct fail-loud serial diagnostics for the three `??`/omission causes in the read path;
a named constant for the duplicated 16×`??` placeholder; a boyscout fix of the null-buffer
`PICC_WakeupA(nullptr, nullptr)` post-read re-select. A native regression that observes the
read-fail diagnostic (captured serial) under an auth-OK/read-fail mock, and the no-key omission
diagnostic under an all-auth-fail mock.

**Out:** using the Gen1a backdoor to read (Step 2, its own slice); changing the `.nfc` dump
structure (no-key sectors stay omitted, not placeholder-filled); the REQA keep-alive re-select in
the Key A loop (`PICC_IsNewCardPresent()` is ineffective on an ACTIVE tag — logged in the ledger
for Step 2's deeper state-machine pass, not touched here, because a behaviour change I cannot yet
verify would violate fail-loud).

## Definition of Done

- **Given** a sector whose dictionary key authenticates but whose block read returns non-OK
- **When** the audit streams that sector with `readData` on
- **Then** each failing block is logged as a READ failure (auth OK, read status code), distinct
  from an auth failure, and is still placeholdered `??` in the dump

- **Given** a sector for which no dictionary key is found, with `readData` on
- **When** the audit processes that sector
- **Then** it logs that the sector is omitted from the dump for lack of a key (4 data blocks
  unreadable), instead of dropping it silently

## Design

In the per-block read loop, split the single silent `??` path into the two branches that already
exist and give each a distinct `Serial.printf` naming the sector, the block address and the status
code that failed (auth vs read). Add an `else` branch for the `readData && dumpFile` case when no
key was found that logs the sector omission once. Extract the repeated 16×`??` byte sequence to a
named `UNKNOWN_BLOCK_HEX` constant (it appears four times). Replace the `PICC_WakeupA(nullptr,
nullptr)` re-select after the block reads with a real 2-byte ATQA buffer, matching every other
call site — a null buffer is UB if the driver dereferences the size, and a silent re-select miss
there cascades into the next sector.

Serial output is the observable, so the native `MockSerial` gains a capture buffer
(`g_mockSerialLog` + `mockSerialClear()`) the regression can assert against; a `g_mockReadFails`
seam makes the mock's `MIFARE_Read` fail after a successful auth.

## Verification

- **Native (`test_rfid_audit`):** `test_audit_reports_block_read_failure` sets `g_mockReadFails`,
  runs with `readData` on and a temp dump path; the captured serial contains `READ FAILED` for the
  authed blocks and no `AUTH FAILED` — **fails before** (both branches were silent, so neither
  marker exists) and passes after. `test_audit_reports_no_key_sector_omitted` sets
  `g_mockAuthFails`; the captured serial contains the omission diagnostic — **fails before** (only
  the terse per-sector line existed). Full suite stays green.
- **On-device:** re-run the audit on `23FF9C1E` with Read Block Data on; the serial log now
  attributes every `??` and every missing sector, producing the evidence for the Step-2
  backdoor-read decision.

## As built

**Code (2026-10-10).** `RfidAudit::runDictionaryAttack()`'s read loop now emits a distinct
fail-loud `Serial.printf` for each `??` cause: `READ FAILED (auth OK, MIFARE_Read status N)` when a
block authenticates but will not read, `AUTH FAILED on read-back (key cracked but re-auth missed,
status N)` when a cracked sector's per-block re-auth misses, and `Sector N: no dictionary key found
-> omitted from dump` for a sector dropped for lack of a key. The duplicated 16×`??` / 6×`??`
placeholders are now the `UNKNOWN_BLOCK_HEX` / `UNKNOWN_KEY_HEX` constants, and the post-read
re-select passes a real ATQA buffer instead of `PICC_WakeupA(nullptr, nullptr)`. The native
`MockSerial` captures into `g_mockSerialLog` (`mockSerialClear()`); `mfrc522_mocks.h` gains a
`g_mockReadFails` seam.

The REQA keep-alive re-select (`PICC_IsNewCardPresent()` in the Key A loop) was left untouched and
logged in the ledger for Step 2 — it is ineffective on an ACTIVE tag, but changing it is an
unverified behaviour change outside this slice's diagnostics-only scope.

**Verified in-conversation (2026-10-10):**
- `pio test -e native -f test_rfid_audit` → 5/5; full suite `pio test -e native` → 802/802.
- Fails-before proof: `git stash` of `rfid_audit.cpp` (HEAD's silent `??` paths) makes both new
  tests fail (`Expected TRUE Was FALSE` — neither `READ FAILED` nor `omitted from dump` is emitted);
  restored, both pass.
- `pio run -e cardputer` → SUCCESS (diagnostics are log strings only; negligible flash).

**On-device (2026-10-11):** verified. Re-running the audit on `23FF9C1E` with Read Block Data on
attributed every `??`: `Sector N block M: AUTH FAILED on read-back (... status 2)` for blocks 2-4
of all 16 sectors (block 1 read). That evidence root-caused the per-block re-auth desync and
directly drove slice-0046 — exactly the purpose of this diagnostics slice.
