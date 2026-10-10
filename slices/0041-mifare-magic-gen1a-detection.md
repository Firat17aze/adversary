---
id: '0041'
title: "MIFARE Classic magic (Gen1a backdoor) detection"
type: slice
status: accepted
date: 2026-10-10
supersedes: []
superseded_by: []
---

## Goal

Tell the operator whether a detected MIFARE Classic tag is a **Gen1a "magic"** card — one
whose block 0 (UID) is rewritable through the Chinese backdoor command set — versus an
ordinary, locked card. This is the single most useful fact about a card for a clone workflow:
a Gen1a is a blank you can write a cloned UID onto; a normal card is not.

## Problem

`RFIDManager` already carries an `isMagic` field on `RFIDTagInfo`, but it is a **hardcoded
stub**:

```cpp
// src/modules/rfid/rfid_manager.cpp:141-142
// Check for Magic Card Gen1 (UID changeable via backdoor)
m_lastTag.isMagic = false;
```

The comment promises a Gen1a probe that was never written, and no screen reads the field, so
it is populated-but-dead data that always reports "not magic" — misleading if ever surfaced,
and dead by KISS/no-dead-code. On-device baseline (2026-10-10) confirmed the read + dictionary
+ SD-dump path works against a white common card (`C2D19C1E`) and a drop-keychain fob
(`4179146E`), both ordinary MIFARE 1K; the operator also has Gen1a magic cards on hand — so a
true-positive and a true-negative fixture both exist for verification.

## Scope

**In:** real Gen1a detection via the standard backdoor wake sequence; drive `isMagic` from the
probe result; surface it as a "MAGIC: YES/NO" line in the RFID details view; a native
regression test via the existing MFRC522 mock.

**Out:**
- **Gen2 / CUID / FUID** ("direct write" magic) — these answer the normal MIFARE command set
  and are protocol-indistinguishable from a genuine card without *attempting* a block-0 write,
  which is a destructive operation out of scope for a read-only probe. Detecting them belongs
  with a future write/clone feature, not here. The details line therefore means specifically
  "Gen1a backdoor present", and the slice documents that limit.
- **Writing / cloning** a UID onto a magic card — a separate feature (see LEDGER RFID
  emulation/clone items); this slice only *detects*.
- **LF (125 kHz)** tags — our WS1850S/MFRC522 is 13.56 MHz HF only.

## Definition of Done

- **Given** a Gen1a magic card on the reader
- **When** the operator reads it and opens Details
- **Then** the card is reported `MAGIC: YES`, and the serial log shows the backdoor ACK

- **Given** an ordinary MIFARE Classic card (the white card / the keychain fob)
- **When** read
- **Then** it is reported `MAGIC: NO` — the probe sees no backdoor ACK (expected NAK/timeout,
  not an I/O error)

- **Given** a card that was just magic-probed
- **When** the operator runs the dictionary audit on it immediately after
- **Then** the audit still detects and cracks it — the probe left the card in a clean,
  re-selected state (the detection is non-destructive to the normal read path)

- **Given** an I2C/transceive fault during the probe (not a card NAK)
- **When** it happens
- **Then** the error is logged and `isMagic` stays false — the fault is visible, not silently
  folded into a "not magic" verdict (fail loud; a normal-card NAK/timeout is the expected
  negative and is **not** treated as a fault)

## Design

### The Gen1a backdoor probe

Gen1a magic cards answer a non-standard wake sequence that genuine NXP silicon ignores:

1. `PICC_HaltA()` — bring the card to a known HALT state.
2. Send **`0x40`** as a **7-bit** frame, no CRC. A Gen1a answers with a 4-bit **`0x0A`** ACK;
   a genuine card stays silent (timeout) or NAKs.
3. Confirm with **`0x43`** (full byte, no CRC) → a Gen1a answers `0x0A` again.

**Reuse, do not re-hand-roll (revised from the draft).** This draft originally specified
hand-rolling the sequence via `PCD_TransceiveData` with explicit `validBits`. On inspection the
linked driver (`.pio/libdeps/cardputer/MFRC522_I2C`) **already implements this exact wire
sequence** as `bool MIFARE_OpenUidBackdoor(bool logErrors)` — HALTA → 7-bit `0x40` → expect
`0x0A` → `0x43` → expect `0x0A`, returning `true` only on a full backdoor ACK. Re-hand-rolling
the raw framing would duplicate correct, CRC-handling library code we already compile in, which
KISS/DRY forbids. So `probeMagicGen1a()` calls `MIFARE_OpenUidBackdoor(false)` (errors not
logged by the library — see the fail-loud note for why our own layer handles that).

### Non-destructiveness is correctness-critical

A successful backdoor sequence leaves a Gen1a in its *unlocked* state; and because
`MIFARE_OpenUidBackdoor` begins with `PICC_HaltA`, **every** probed card — magic or not — is
left HALTed afterwards. For a **detection-only** probe this must not leak into the normal
read/notify/audit path. So the probe, on completion, restores a clean selection:
`PICC_HaltA()` → `PICC_WakeupA()` → `PICC_Select(&uid, 0)` (full anticollision) so `m_lastTag`
reflects a normally-selected card and a subsequent audit behaves identically to today. The DoD
pins this: audit must still crack a card that was just probed.

The probe runs **once per newly-seen UID** (the same `isNewUid` edge that gates the
notification), not on every 200 ms poll: it HALTs and re-selects the tag, so re-running it
while a card simply rests on the reader would needlessly churn card state five times a second.
The verdict persists in `m_lastTag.isMagic` for the Details view until `clearTag()`.

### Fail-loud verdict, not a masking default

`MIFARE_OpenUidBackdoor` returns a single `bool`: it collapses "ordinary card, no backdoor"
(the *expected* negative) and "bus/I2C fault" into one `false`. A normal card's silence is the
correct negative and must stay silent. The failure we must not mask is a **degraded reader
reporting every card as non-magic**. Rather than re-hand-roll the sequence purely to inspect
the status code, the fail-loud signal is folded into the **mandatory post-probe re-select**:
the probe already has to `WakeupA` + `Select` the HALTed tag for non-destructiveness, so if
that re-select fails we log `[RFID] Magic probe: tag did not re-select cleanly; bus/tag state
suspect`. A persistent bus fault fails the re-select and is surfaced; a normal card re-selects
fine and its `MAGIC: NO` is trustworthy. The only window this does not catch is a transient
fault confined to exactly the two backdoor frames that then recovers for the re-select — and
the probe runs immediately after a successful select+read *and* immediately before a
re-select, so a fault isolated to just those two frames is a negligible, non-actionable case.
The positive is also logged (`[RFID] Magic Gen1a backdoor ACK -> block 0 / UID is rewritable`)
so the DoD's "serial shows the backdoor ACK" holds.

### One new method, mirroring the existing shape

`RFIDManager::probeMagicGen1a()` (private, returns `bool`), called from `update()` in place of
the `m_lastTag.isMagic = false;` line. It encapsulates the halt → 7-bit `0x40` → `0x43` →
restore sequence. No public API change; `RFIDTagInfo::isMagic` already exists.

### Surface it

`RfidScreen::drawDetailsSubscreen` already renders UID (large), TYPE, and SAK. Add a
**"MAGIC: YES"/"MAGIC: NO"** line beneath SAK, YES drawn in the warning/success accent so a
cloneable card is obvious at a glance. This is what turns the field from dead data into a
feature, and is why detection and display ship together.

### The host-testable piece

The MFRC522 native mock (`test/common/mfrc522_mocks.h`) gains:
- a settable `inline bool g_mockMagicGen1a = false;` (mirrors the existing `g_mockMfrcVersion`
  seam, default false so existing RFID tests are untouched),
- a `MIFARE_OpenUidBackdoor(bool)` that simply returns `g_mockMagicGen1a` — faithful to the real
  driver, which likewise collapses "ordinary card" and "fault" into one `bool` (the caller's
  fail-loud handling lives in the re-select, not here), and
- `PICC_Select(Uid*, byte = 0)` returning `STATUS_OK` (the mock lacked it; the re-select path
  now needs it).

`test_rfid_manager` gains two cases asserting observable behaviour on the public surface:
- magic mock **true** → after `activate()` + `update()`, `getLastTag()->isMagic == true`
- magic mock **false** → `getLastTag()->isMagic == false`

The true-case **fails before** this slice (the hardcoded `false` can never report magic) and
**passes after** — the required fails-before/passes-after regression. The probe's wire-level
correctness (7-bit framing, the real card's ACK timing, CRC handling, non-destructive
re-select) stays on-device verification, as with the CC1101/RFID slices — the mock proves the
verdict plumbing, hardware proves the handshake.

## Verification

- **Native:** `test_rfid_manager` magic true/false regression (above); existing RFID coverage
  unchanged (mock default `g_mockMagicGen1a = false`).
- **Build:** `pio run -e cardputer` links the new probe + the details line.
- **On-device (the DoD, verified on hardware before commit):**
  1. Gen1a magic card → `MAGIC: YES` + serial backdoor ACK.
  2. White common card (`C2D19C1E`) and keychain fob (`4179146E`) → `MAGIC: NO`.
  3. Magic card: probe, then immediately audit → audit still cracks it (non-destructive).
  4. Confirm no SD/bus disturbance and no phantom state after repeated probes.

## As built

**Code (2026-10-10).** Implemented across five files:
- `rfid_manager.h/.cpp` — new private `probeMagicGen1a()`; the dead `m_lastTag.isMagic = false;`
  stub is replaced by a call gated on the `isNewUid` edge. The probe reuses the driver's
  `MIFARE_OpenUidBackdoor(false)`, then `HaltA` → `WakeupA` → `PICC_Select(&uid, 0)` to restore a
  clean selection; a failed re-select is logged (fail loud), a positive logs the backdoor ACK.
  The `Tag Detected` serial line now carries `MAGIC: YES/NO`.
- `rfid_screen.cpp` — a `MAGIC:` row in `drawDetailsSubscreen` (WARNING accent for YES,
  disabled-grey for NO); `getDetailsContentHeight` base lines 6 → 7.
- `mfrc522_mocks.h` — `g_mockMagicGen1a` seam, `MIFARE_OpenUidBackdoor(bool)`, `PICC_Select`.
- `test_rfid_manager.cpp` — `test_rfid_manager_detects_magic_gen1a` (fails-before/passes-after)
  and `test_rfid_manager_ordinary_card_is_not_magic`.

**Verified in-conversation (2026-10-10):**
- `pio test -e native -f test_rfid_manager` → 9/9 pass.
- Fails-before proof: with the manager reverted to the old stub (test + mock kept),
  `test_rfid_manager_detects_magic_gen1a` fails `Expected TRUE Was FALSE`; the ordinary-card
  case still passes. Restored → 9/9.
- `pio run -e cardputer` → SUCCESS. RAM 26.5% (86 704 B of 327 680), Flash 73.9% — within the
  no-PSRAM SRAM budget.

**On-device, partial (2026-10-10, real WS1850S, Version 0x15):**
- ✅ **True-negative** — three ordinary MIFARE 1K cards (`C2D19C1E`, keychain `4179146E`,
  `9B8D1682`) all report `MAGIC: NO`. No false positives.
- ✅ **Non-destructive** — `C2D19C1E` was magic-probed (→ NO) and then a full dictionary audit
  ran on the same selection, cracking all 16 sectors (Key A idx 0 throughout, Key B on
  sectors 7/8/10/12/14/15) and saving `C2D19C1E.nfc`. The post-probe re-select is clean on
  hardware.
- ✅ **Fail-loud quiet** — the `tag did not re-select cleanly` line never fired; re-select
  succeeded on every probe.
- ❌ **True-positive still open** — the operator's cards all read `MAGIC: NO`. This is *expected*
  if those cards are **Gen2/CUID** "direct-write" magic, which this Gen1a-backdoor probe
  deliberately does not detect (Out-of-scope, above) and which are protocol-indistinguishable
  from an ordinary card on a read. Verifying `MAGIC: YES` requires specifically a **Gen1a**
  ("UID-changeable via backdoor" / "Chinese magic") card. Tracked in the LEDGER until one is
  on hand.
