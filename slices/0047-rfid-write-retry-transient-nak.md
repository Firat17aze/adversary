---
id: '0047'
title: "Bounded write-retry on a transient MIFARE write NAK/timeout"
type: slice
status: accepted
date: 2026-10-11
supersedes: []
superseded_by: []
---

## Goal

A clone must land every data block on the **first** write-back pass. A transient RF warm-up
miss on an early block must self-recover, not leave a gap the operator only closes by running
the write a second time.

## Problem

Field evidence (card `23FF9C1E`, slice-0046 write-back, 2026-10-11): the **first** write-back
after the antenna toggles on dropped 5 data blocks in the first two sectors to `status 4`
(STATUS_TIMEOUT) despite a good sector auth; an immediate second run wrote 47/47. The failure is a
transient RF warm-up on the first transceives after the field comes up, not a logic bug — but a
one-shot write surfaces it as 5 permanent `failedWrite`s, and a clone that needs a second pass to
be complete is a clone that is wrong on the first.

The writer ([rfid_writer.cpp](../src/modules/rfid/rfid_writer.cpp)) issues exactly one
`MIFARE_Write` per block and records any non-OK status as a permanent `failedWrite`. Every other
fragile RF step in the module already retries with a re-select between tries — `selectCard()` and
the audit select loop (`for (retry < 5)`, escalating backoff) and the Gen1a magic probe
(`MAGIC_PROBE_ATTEMPTS = 5`, re-select to ACTIVE between tries, slice-0044). The block write is the
one fragile step with no such recovery.

## Scope

**In:** a bounded retry around the per-block `MIFARE_Write`, re-selecting and re-authenticating the
block's sector between attempts (a timed-out write leaves the crypto session in an uncertain state;
per slice-0046 a re-auth without a re-select desyncs, so a bare retry would hit the same wall). A
mock seam that fails the first N writes then succeeds, plus a native regression impossible to pass
with the one-shot write.

**Out:** the block-0 / UID write (a single deterministic Gen2/CUID probe, not a data restore — a
lock there is the answer, not a transient to retry); classifying a NAK as transient vs permanent
(we do not — the bounded budget recovers a transient and still fails loud on a permanent one);
changing how `failedWrite` is reported once the budget is spent.

## Definition of Done

- **Given** a write plan whose first data-block write returns a transient NAK/timeout the first
  time (and the next time succeeds)
- **When** the writer restores that plan
- **Then** the block is re-selected, re-authenticated and re-written within a bounded budget and
  counted in `blocksWritten`, instead of being recorded as a permanent `failedWrite`

- **Given** a block whose write never succeeds within the budget (a genuinely locked/deterministic
  failure)
- **When** the writer exhausts its retries
- **Then** it records one `failedWrite` and logs the final NAK, exactly as before — bounded retry
  does not mask a real failure

## Design

Replace the single `MIFARE_Write` in `restore()`'s data-block branch with a bounded loop of
`WRITE_ATTEMPTS = 3`. The first attempt writes under the sector's already-open session. On a
non-OK status, before the next attempt re-`selectCard()` and `authBlock()` the **same** sector,
then retry — one uniform recovery path (re-select → re-auth → write). Re-authing the same sector
keeps the loop's `authedSector` tracking valid, so the sector's remaining blocks continue under the
freshly opened session. If the re-auth itself fails (a deterministic auth loss, not a transient
write miss) stop retrying immediately. Each failed attempt logs the sector, block, attempt number
and status; the final outcome is counted once (`blocksWritten` on eventual OK, else `failedWrite`),
so a spent budget still fails loud.

`WRITE_ATTEMPTS` is 3, not the probe/select loops' 5: a write is far heavier than a wake, the field
miss cleared on the very next pass, and a genuinely locked block should not stall three extra full
re-select+re-auth cycles, let alone five.

**Mock:** `g_mockWriteFailFirstN` makes `MIFARE_Write` return `STATUS_TIMEOUT` for its first N calls
then OK, reproducing the antenna warm-up. Off by default (0), so every existing test is untouched.

## Verification

- **Native (`test_rfid_writer`):** `test_writer_retries_transient_write_nak` sets
  `g_mockWriteFailFirstN = 2` and a small plan, and asserts every planned block is in
  `blocksWritten` with `failedWrite == 0` — **fails before** (the one-shot write records the first
  two blocks as permanent `failedWrite`) and passes after. Full suite stays green; `pio run -e
  cardputer` builds.
- **On-device:** restore the `23FF9C1E` dump onto a blank card on the **first** pass after powering
  the antenna and confirm 47/47 data blocks land without a second run.

## As built

**Code (2026-10-11).** `restore()`'s data-block branch now writes under a bounded
`WRITE_ATTEMPTS = 3` loop: the first attempt uses the sector's open session; on a non-OK status it
re-`selectCard()`s and re-`authBlock()`s the **same** sector (keeping `authedSector` valid for the
sector's remaining blocks) and retries. A re-auth that fails mid-retry is a deterministic auth loss,
not a transient write miss, so the loop stops and the block is counted `failedAuth`; a budget spent
on write NAKs is counted `failedWrite` with a final log line — both still fail loud. Each failed
attempt logs sector/block/attempt/status. `mfrc522_mocks.h` gains `g_mockWriteFailFirstN` (the first
N `MIFARE_Write` calls return `STATUS_TIMEOUT` without recording, modelling the antenna warm-up).

**Verified in-conversation (2026-10-11):**
- `pio test -e native -f test_rfid_writer` → 7/7; full suite `pio test -e native` → 805/805.
- Fails-before proof: `git stash` of `rfid_writer.cpp` (the one-shot write) with
  `g_mockWriteFailFirstN = 2` makes `test_writer_retries_transient_write_nak` fail —
  `Expected 5 Was 3` (the two warm-up misses are recorded as permanent `failedWrite`, so only 3 of
  5 blocks land). Restored; passes (5/5 blocks written, 0 failed).
- `pio run -e cardputer` → SUCCESS (RAM 26.5%, Flash 74.3%, unchanged).

**On-device (2026-10-11):** restored the `23FF9C1E` dump (`64 blocks, 47 writable`) onto a card on
the first write-back pass after the antenna powered on: **`47/47 written, 0 auth-fail, 0
write-fail`** with no second run. No `attempt N/3 NAK` lines appeared, so this particular run did
not hit a warm-up miss — the retry path was not exercised on-device here (the transient is
non-deterministic: the slice-0046 run dropped 5 blocks, this one dropped none). The run therefore
confirms the change does not regress the clean first pass; the retry's recovery behaviour under an
actual transient is the native regression's job (`test_writer_retries_transient_write_nak`,
fails-before `Expected 5 Was 3`).
