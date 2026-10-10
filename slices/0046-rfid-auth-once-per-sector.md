---
id: '0046'
title: "Authenticate once per sector for MIFARE block I/O (audit read + write-back)"
type: slice
status: accepted
date: 2026-10-10
supersedes: []
superseded_by: []
---

## Goal

Reading or writing every block of a cracked MIFARE Classic sector must succeed on the real
reader, not just the first block of each sector. A full 1K card with known keys must round-trip
all 47 data blocks, not ~16.

## Problem

Field evidence (card `23FF9C1E`, slice-0045 diagnostics, 2026-10-10): the dictionary cracks **all
16 sectors** with the factory key, yet the audit dump has only the first block of each sector and
48 `??`:

```
Sector 0: Key A FOUND (key idx 0)
Sector 0 block 1: AUTH FAILED on read-back (key cracked but re-auth missed, status 2) -> ?? in dump
Sector 0 block 2: AUTH FAILED ...
Sector 0 block 3: AUTH FAILED ...
... identical for all 16 sectors ...
```

The cause is a misuse of the MIFARE protocol, not keys or magic cards (this retires the
"backdoor-assisted read" direction floated after slice-0045 — there was nothing for a backdoor to
rescue). One `PCD_Authenticate` authenticates the **whole sector**; the correct pattern is
authenticate once, read/write all four blocks, `PCD_StopCrypto1` once. Both the audit read loop
([rfid_audit.cpp](../src/modules/rfid/rfid_audit.cpp)) and the writer
([rfid_writer.cpp](../src/modules/rfid/rfid_writer.cpp)) instead do `Authenticate → I/O →
StopCrypto1` **per block**. The second authentication to a sector, issued after `StopCrypto1` with
no intervening re-select, desyncs the crypto handshake and the WS1850S NAKs it (`status 2` =
STATUS_ERROR) — so blocks 2-4 of every sector fail.

The writer only half-masked this: `authBlock` re-selects on a failed attempt and falls through
(Key A → Key B → factory), so a sector with a known key recovers on the 2nd attempt. But a sector
whose dump keys are all `??` runs the factory attempt alone, hits the desync with no fallback left,
and fails its first block — round 1's "8/15 written, 7 sectors auth-failed".

## Scope

**In:** audit read and writer both authenticate once per sector and keep the crypto session open
across that sector's blocks, re-selecting the card cleanly at each sector boundary and calling
`StopCrypto1` once per sector. A mock seam modelling the re-auth-needs-reselect desync, plus a
native regression for each path that is impossible to pass with the per-block pattern.

**Out:** Key B discovery (the dictionary's `A-as-B` probe and full B scan are unchanged — a
separate concern); the Key-A-loop REQA keep-alive (still deferred, slice-0045 ledger note); any
Gen1a-backdoor read path (shown unnecessary for an ordinary-keyed card).

## Definition of Done

- **Given** a MIFARE 1K card whose sectors are cracked but whose reader NAKs a re-auth issued
  without a re-select
- **When** the audit reads block data with `readData` on
- **Then** all four blocks of every cracked sector are read (no `??` from a re-auth miss), because
  the sector is authenticated once and read under that single session

- **Given** a write plan whose target sector exposes only the factory key (dump keys `??`)
- **When** the writer restores that sector's data blocks
- **Then** every planned block in the sector is written under one authentication, instead of the
  sector's first block failing auth with no fallback

## Design

**Audit read:** replace the per-block `Authenticate/StopCrypto1` with: re-select the card, one
`PCD_Authenticate` against the sector's first block with the chosen key, then `MIFARE_Read` all
four blocks under that session, `PCD_StopCrypto1` once. A whole-sector auth failure logs once and
placeholders all four blocks; an individual read failure keeps its per-block `READ FAILED`
diagnostic (slice-0045). The now-redundant end-of-read re-select is removed — each sector's read
re-selects at its own start, and the next sector's dictionary already re-selects.

**Writer:** walk the (addr-ordered, so sector-contiguous) write plan tracking the current sector;
on a sector change, `StopCrypto1` the previous sector, `selectCard`, and `authBlock` once; write
each block of the sector with no intervening re-auth or `StopCrypto1`; `StopCrypto1` once when the
sector changes or the plan ends. A sector whose auth fails marks its planned blocks `failedAuth`
and skips them.

**Mock:** `g_mockReauthNeedsReselect` makes `PCD_Authenticate` NAK a second auth unless a re-select
(`WakeupA`/`RequestA`/`ReadCardSerial`/`Select`) has happened since the last successful auth —
faithfully reproducing the field desync. Off by default, so every existing test is untouched.

## Verification

- **Native (`test_rfid_audit`):** `test_audit_reads_all_blocks_under_single_auth` sets
  `g_mockReauthNeedsReselect`, runs with `readData` on, and asserts the streamed dump contains no
  `??` — **fails before** (per-block re-auth NAKs blocks 2-4 of every sector) and passes after.
- **Native (`test_rfid_writer`):** `test_writer_writes_all_blocks_in_factory_only_sector` sets the
  desync and a dump with unknown keys (factory-only), and asserts every planned block is written
  (no `failedAuth`) — **fails before** (the sector's first block fails the lone factory attempt).
- Full suite stays green; `pio run -e cardputer` builds.
- **On-device:** re-run the audit on `23FF9C1E` with Read Block Data on — all four blocks of each
  cracked sector now read; then restore the dump onto a blank card and confirm the data blocks
  round-trip.

## As built

**Code (2026-10-11).** The audit read loop (`rfid_audit.cpp`) now re-selects the card, issues one
`PCD_Authenticate` against the sector's first block, reads all four blocks under that session, and
calls `PCD_StopCrypto1` once; a whole-sector auth failure logs once and placeholders all four
blocks, an individual read miss keeps its slice-0045 `READ FAILED` diagnostic, and the redundant
end-of-read re-select is gone. The writer (`rfid_writer.cpp`) tracks the current sector across the
addr-ordered plan, `selectCard`+`authBlock`s once per sector, writes every block of the sector
with no intervening re-auth/`StopCrypto1`, and `StopCrypto1`s once on each sector boundary and at
the end. `mfrc522_mocks.h` gains the `g_mockReauthNeedsReselect` desync model (a successful auth
consumes the selection; the next auth NAKs until a re-select).

**Verified in-conversation (2026-10-11):**
- `pio test -e native -f test_rfid_audit -f test_rfid_writer` → 12/12; full suite 804/804.
- Fails-before proof: `git stash` of `rfid_audit.cpp`+`rfid_writer.cpp` (per-block pattern) with
  the desync model active makes both new tests fail —
  `test_audit_reads_all_blocks_under_single_auth` → `Expected TRUE Was FALSE` (dump still has `??`),
  `test_writer_writes_all_blocks_in_factory_only_sector` → `Expected 4 Was 2` (sector's first block
  fails the lone factory auth). Restored; both pass.
- `pio run -e cardputer` → SUCCESS (RAM 26.5%, Flash 74.3%, unchanged).

**On-device (2026-10-11):** verified on `23FF9C1E` (a first HALT-less re-select attempt timed out
with `status 4`, fixed by HALT-before-WUPA, then):
- Audit read: **0 AUTH FAILED, 0 READ FAILED, 0 omitted** across all 16 sectors (was 48 `??`); the
  dump loaded as `64 blocks, 47 writable` — every data block captured. Key B is now discovered on
  sectors 1-15 too (the old desync had corrupted key discovery as well).
- Write-back: **0 auth-fail** (round 1 had 7 sectors fail) and, on a settled run, **47/47 data
  blocks written**. A first write attempt after the antenna toggles on dropped 5 blocks in the
  first two sectors to `status 4` write timeouts — a transient RF warm-up, not this slice's logic
  (a second run wrote 47/47). First-attempt reliability is addressed by a bounded write retry in
  slice-0047.

The HALT-before-WUPA re-select is on-device-verified rather than mock-tested: the native mock does
not model "an ACTIVE tag ignores WUPA" (a wire-protocol timing detail), and a second overlapping
state machine for it would risk its own bugs for little gain.
