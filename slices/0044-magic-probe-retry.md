---
id: '0044'
title: "Make Gen1a magic detection reliable with a bounded backdoor retry"
type: slice
status: accepted
date: 2026-10-10
supersedes: []
superseded_by: []
---

## Goal

A Gen1a magic card must report `MAGIC: YES` reliably, not intermittently. A single card placed on
the reader twice in one session must not read `NO` then `YES`.

## Problem

`RFIDManager::probeMagicGen1a()` (slice-0041) makes a **single** `MIFARE_OpenUidBackdoor()`
attempt and treats one failure as "not magic". The backdoor wake is timing/state-sensitive —
`HALT → 0x40 (7-bit) → expect 0x0A → 0x43 → expect 0x0A`, and the first transceive after HALT
frequently misses on the first try. Observed in-field 2026-10-10: the same Gen1a card
(`23FF9C1E`) read `MAGIC: NO` on one detection and `MAGIC: YES` on the next within one session.

The error is one-directional and that is the whole point: a **YES** requires the card to ACK the
non-standard backdoor frames, which ordinary NXP silicon physically will not do, so YES is
high-confidence. A **NO** only means "did not ACK *this attempt*", which can be a flaky miss. A
single-shot probe therefore produces false negatives, never false positives.

This invalidates a slice-0041 inference: the operator's earlier cards reading `MAGIC: NO` were
taken as evidence they were Gen2/CUID. With false negatives on the table, a single negative probe
says nothing about card *type* — those cards may well be Gen1a. (LEDGER corrected.)

## Scope

**In:** retry the backdoor open a bounded number of times inside `probeMagicGen1a()`, re-selecting
the card to ACTIVE between attempts; return `YES` on the first ACK, `NO` only after N clean
failures. A native regression driving a mock that ACKs only on a later attempt.

**Out:** the Gen2/CUID question (still answered only by attempting a block-0 write, slice-0043);
changing the end-of-probe re-select / fail-loud behaviour (unchanged).

## Definition of Done

- **Given** a card whose backdoor ACKs only on the 2nd attempt (the 1st misses)
- **When** `probeMagicGen1a()` runs once for that newly-seen UID
- **Then** it reports `isMagic == true` — the retry recovers the false negative a single-shot
  probe would have reported as `NO`

- **Given** an ordinary card that never ACKs the backdoor
- **When** the probe runs
- **Then** it reports `isMagic == false` after exhausting the retries, and the card still
  re-selects cleanly for a follow-up audit (no regression to slice-0041's non-destructive
  guarantee)

## Design

`probeMagicGen1a()` wraps the `MIFARE_OpenUidBackdoor()` call in a loop of `MAGIC_PROBE_ATTEMPTS`
(a named constant). Attempt 0 runs against the card as `update()` left it (freshly selected);
each later attempt first re-selects the card to ACTIVE (`HaltA → WakeupA → ReadCardSerial`) so the
backdoor starts from the same clean state a fresh detection would, then calls the backdoor again.
The loop exits on the first `true`. The existing end-of-probe re-select (keep the tag live for an
immediate audit) and its fail-loud "suspect" log are unchanged.

## Verification

- **Native (`test_rfid_manager`):** a new `g_mockMagicAckOnAttempt` seam makes the mock's
  `MIFARE_OpenUidBackdoor` return false until the Nth call. With it set to 2, `update()` →
  `probeMagicGen1a()` reports `isMagic == true`; this **fails before** (single-shot sees only the
  1st false) and passes after. The existing ordinary-card (`g_mockMagicGen1a=false`) test still
  reports `NO`, guarding against a false positive.
- **On-device:** place the Gen1a card several times; `MAGIC: YES` every time, no `NO` flips.

## As built

**Code (2026-10-10).** `RFIDManager::probeMagicGen1a()` now loops `MAGIC_PROBE_ATTEMPTS = 5`
times over `MIFARE_OpenUidBackdoor()`, re-selecting the card to ACTIVE (`HaltA → WakeupA →
ReadCardSerial`) before each retry and exiting on the first ACK; the end-of-probe re-select and
fail-loud "suspect" log are unchanged. `mfrc522_mocks.h` gains `g_mockMagicAckOnAttempt` /
`g_mockBackdoorCalls`; `test_rfid_manager` gains
`test_rfid_manager_magic_probe_retries_past_false_negative`.

**Verified in-conversation (2026-10-10):**
- `pio test -e native -f test_rfid_manager` → 10/10.
- Fails-before proof: reverting the loop to a single `MIFARE_OpenUidBackdoor()` call makes the
  new test fail (`Expected TRUE Was FALSE` — the 1st attempt's miss is reported as NO); the
  retry passes.

**On-device:** pending — place the Gen1a card (`23FF9C1E`) several times and confirm `MAGIC: YES`
every time with no `NO` flips.
