---
id: '0043'
title: "RFID Write: restore a .nfc dump onto a MIFARE Classic card"
type: slice
status: accepted
date: 2026-10-10
supersedes: []
superseded_by: []
---

## Goal

Turn the `RFID Write (Coming Soon)` placeholder into a working feature: take a `.nfc` dump
produced by the audit and write it back onto a MIFARE Classic 1K card — the data blocks
restored with the cracked keys, and, as an explicit opt-in, the UID (block 0) attempted for
magic cards. The operator can re-provision or clone a card without a PC.

## Problem

The driver already ships every write primitive (`MIFARE_Write` for data blocks,
`MIFARE_SetUid` / `MIFARE_UnbrickUidSector` for the Gen1a UID backdoor), and the audit
(`rfid_audit.cpp`) already cracks keys and streams a Flipper-style `.nfc` dump
(`Filetype: Flipper NFC device` / `Version: 4`, per-sector `Block N:` lines plus `Key A:` /
`Key B:`). What is missing is the reverse direction: nothing parses a dump back into blocks or
writes them to a card. The menu entry is a static `MenuItem::disabled("RFID Write (Coming
Soon)")`.

## Scope

**In:**
- A pure, native-testable `.nfc` parser + write-plan builder (`rfid_dump.{h,cpp}`): parse the
  Version-4 streamed format into UID / SAK / per-block bytes / per-sector keys, then classify
  every block and build the list of blocks that are *safe* to write.
- A thin hardware writer (`rfid_writer.{h,cpp}`, sibling to `RfidAudit`, takes `MFRC522_I2C*`):
  authenticate per sector and `MIFARE_Write` each planned data block; fail loud on auth/write
  failure.
- An explicit, opt-in UID (block 0) write step that **fails loud** on an ordinary card's locked
  manufacturer block — which doubles as the definitive Gen2/CUID direct-write test for the
  operator's `MAGIC: NO` cards (see slice-0041's open verify item).
- UI: a `.nfc` file picker and a write/confirm/progress/summary flow in `rfid_screen.cpp`,
  reached from a new `w` action; retire the disabled menu placeholder.

**Out:**
- **Writing sector trailers (blocks 3, 7, 11, …).** A wrong access-bits byte in a trailer is an
  **irreversible brick** of that sector, and the audit's trailer read-back returns the key
  fields as zeros (keys are never readable over the air), so a faithful trailer restore is not
  even possible from a dump. v1 **always skips** trailers and reports how many it skipped. A
  later slice can add a guarded "install keys / access bits" path with brick warnings.
- **Cross-card key installation.** Because trailers are not written, a restore authenticates
  with the key already on the *target* card. The reliable, verifiable-now use case is therefore
  "restore onto the same card (or a card already sharing the dump's keys / a factory-blank
  card)". A true blank-to-cloned-keys clone needs trailer writes — the deferred slice above.
- **Gen1a UID rewrite via the backdoor** (`MIFARE_SetUid`). v1's UID step uses a plain
  `MIFARE_Write` to block 0, which is exactly the Gen2/CUID test. Wiring `MIFARE_SetUid` for
  Gen1a cards is a follow-up gated on actually having a confirmed Gen1a card (still an open
  item from slice-0041).
- Non-1K MIFARE (4K / Mini) and non-MIFARE (SAK & 0x20) cards — rejected up front, as the audit
  already rejects them.

## Definition of Done

- **Given** a Version-4 `.nfc` dump whose sector 1 block 4 reads `11 22 … ` and whose sector 0
  block 2 reads `?? ?? …` (unreadable at audit time)
- **When** a write plan is built from it
- **Then** block 4 is in the plan's data writes with those bytes, block 2 is counted as
  *skipped-unknown* and is **not** written, and no sector-trailer block (3, 7, …) and not block
  0 appear in the data writes

- **Given** that same dump and a card on the reader whose sector keys match the dump
- **When** the restore runs with the UID option off
- **Then** every planned data block is `MIFARE_Write`-ten after a successful per-sector auth,
  the summary reports the written count, and a block whose auth or write fails is counted as a
  failure (never silently skipped)

- **Given** the UID option on and an **ordinary** card (locked block 0)
- **When** the restore attempts the block-0 write
- **Then** the write returns a NAK, the summary reports `UID not written — block 0 locked
  (ordinary / not Gen2-CUID)`, and the data-block writes still complete — the locked UID is an
  expected negative, reported, not an error that aborts the restore or is swallowed

## Design

### Layering (mirrors the audit)

`RfidAudit` is the precedent: a hardware class over `MFRC522_I2C*` with a pure data core. RFID
Write splits the same way, keeping the risky hardware surface thin and the decision logic pure
and native-tested:

- **`rfid_dump.{h,cpp}` — pure, native-compiled (no `<SD.h>`, no Arduino HW).** The file is read
  into a `std::string` by the caller; the parser takes the string. This keeps the parser and the
  classification/plan logic — where every safety decision lives — fully unit-testable off-target.
- **`rfid_writer.{h,cpp}` — hardware glue.** `RfidWriter(MFRC522_I2C*)` with
  `WriteResult restore(const WritePlan&, const NfcDump&, bool includeUid, onProgress)`. It owns
  only card selection, per-sector auth, and `MIFARE_Write`.

### Data model and the classification rule (the heart)

```cpp
enum class BlockKind { DATA, SECTOR_TRAILER, MANUFACTURER };   // MANUFACTURER == block 0
BlockKind classifyBlock(uint8_t addr);   // addr==0 -> MANUFACTURER; addr%4==3 -> TRAILER; else DATA

struct NfcBlock { uint8_t addr; bool known; uint8_t bytes[16]; };  // known==false for "?? .." lines
struct NfcSectorKey { bool keyAKnown; uint8_t keyA[6]; bool keyBKnown; uint8_t keyB[6]; };
struct NfcDump { bool valid; std::string parseError; std::string uid; uint8_t sak;
                 std::vector<NfcBlock> blocks; NfcSectorKey keys[16]; };

struct WritePlanItem { uint8_t addr; uint8_t sector; uint8_t bytes[16]; };
struct WritePlan { std::vector<WritePlanItem> dataWrites; int skippedUnknown; int skippedTrailer;
                   bool hasManufacturer; NfcBlock manufacturer; };
```

`buildWritePlan(dump)` is pure and takes no options — block 0 and trailers are *never* data
writes, so the UID opt-in belongs to the writer, not the plan: for each block, `DATA && known` →
`dataWrites`; `DATA && !known` → `skippedUnknown`; `SECTOR_TRAILER` → `skippedTrailer` (always,
v1); `MANUFACTURER` → stashed in `manufacturer` (written only when the writer is called with
`includeUid`). A naive "write every parsed block" implementation fails all three DoD
block-classification assertions — that is the fails-before/passes-after regression.

`parseNfcDump` **fails loud**: a `Block`/`Key` line with malformed hex sets `valid=false` and a
`parseError`; it never silently drops a byte or a block. A dump produced with the audit's
"Read Block Data" option **off** carries keys but no `Block` lines → `dataWrites` empty, which
the UI reports plainly ("dump has no block data — re-audit with Read Block Data on").

### Write path and key selection (`RfidWriter::restore`)

Per sector that has at least one planned data block: authenticate with the dump's **Key A**, and
on auth failure fall back to **Key B**, then to the factory default `FF FF FF FF FF FF` (so a
freshly blanked target still takes the data). Each fallback is logged. If all fail, every data
block in that sector is counted `failedAuth` and the restore moves on — a locked sector is
reported, not fatal. Each `MIFARE_Write` NAK increments `failedWrite`. Nothing is swallowed.

The UID step (only if `includeUid`): authenticate sector 0, `MIFARE_Write(0, manufacturerBytes)`.
`STATUS_MIFARE_NACK` → `uidWritten=false`, `uidError="block 0 locked (ordinary / not
Gen2-CUID)"`; `STATUS_OK` → `uidWritten=true` ("Gen2/CUID direct-write magic confirmed"). Either
outcome is surfaced in the summary — this *is* the Gen2/CUID determination for a `MAGIC: NO`
card, so it must be reported verbatim, never masked.

### UI flow (`rfid_screen.cpp`)

New states on the existing machine: `WRITE_PICK` (a `ListView` of `*.nfc` under
`/adversary/captures/RFID/`, selection defaulting to the file whose name matches the present
tag's UID), `WRITE_PROMPT` (shows the plan: *N data blocks, M trailers skipped, K unknown
skipped*, and a `Write UID (magic only): [OFF]` toggle), `WRITING` (block progress, same forced
-render pattern as the audit), `WRITE_SUMMARY` (written / failed / UID result). Entry is a new
`w` footer action from `RESULT`/`IDLE`; `buildRootMenuItems()`'s
`MenuItem::disabled("RFID Write (Coming Soon)")` becomes a live action into this flow.

## Verification

- **Native (`test_rfid_dump`):** parse a fixture Version-4 dump (some `??` blocks, mixed
  key availability) and assert the plan — the three DoD block-classification facts, the
  skipped-unknown/trailer counts, and `parseNfcDump` failing loud on malformed hex. The
  block-0/trailer/unknown exclusions are the required fails-before/passes-after regression
  against a naive write-everything plan.
- **Native (`test_rfid_writer`):** extend `mfrc522_mocks.h` with a recording `MIFARE_Write`
  (stores per-block bytes, with a settable block-0 NAK and an all-keys-fail switch) and assert:
  planned data blocks are written after auth; a sector whose dump key is absent still writes via
  the factory-FF fallback; a sector that will not authenticate is counted (`failedAuth`) with
  nothing written; and UID-on against a NAK'ing block 0 yields the "locked / not Gen2-CUID"
  result without aborting the data writes.
- **Build:** `pio run -e cardputer` links the parser, writer, and the new screen states.
- **On-device:** audit one of the ordinary MIFARE 1K cards with Read Block Data on → restore the
  resulting dump onto the same card (UID off) → re-audit/read and confirm a chosen data block
  round-trips. Then restore with UID on → confirm the summary reports block 0 locked (which
  settles the Gen2/CUID question for that card). Tracked in the LEDGER.

## As built

**Code (2026-10-10).** Implemented as designed:
- `src/modules/rfid/rfid_dump.{h,cpp}` — pure parser + `buildWritePlan(dump)` (no `includeUid`
  param: block 0 and trailers are never data writes regardless, so the opt-in lives in the
  writer). `classifyBlock`, `NfcDump`/`NfcBlock`/`NfcSectorKey`, `WritePlan`.
- `src/modules/rfid/rfid_writer.{h,cpp}` — `RfidWriter(MFRC522_I2C*)::restore(plan, dump,
  includeUid, onProgress)`; per-sector Key A→B→factory-FF auth, `MIFARE_Write`, fail-loud counts,
  block-0 attempt as the Gen2/CUID verdict.
- `src/ui/screens/rfid_screen.{h,cpp}` — `WRITE_PICK`/`WRITE_PROMPT`/`WRITING`/`WRITE_SUMMARY`
  states, a `w` footer action from the dashboard, and a `.nfc` file picker over
  `/adversary/captures/RFID/`. A one-shot `RfidScreen::requestWriteMode()` lets the menu entry
  open straight into the picker.
- `src/ui/menu_factory.cpp` / `menu_actions.h` / `menu_routes.cpp` — `RFID Write (Coming Soon)`
  disabled item becomes a live `MenuItem::gated(... ACTION_RFID_WRITE ...)` routed to the RFID
  screen; `src/main.cpp` sets the write-mode intent before navigating.
- `test/common/mfrc522_mocks.h` — recording `MIFARE_Write` plus `g_mockBlock0Nak` /
  `g_mockAuthFails` seams.
- `test/test_rfid_dump/` (5 tests) and `test/test_rfid_writer/` (5 tests).

**Verified in-conversation (2026-10-10):**
- `pio test -e native -f test_rfid_dump -f test_rfid_writer` → 10/10; full `pio test -e native`
  → 799/799.
- Fails-before proof: a naive "write every parsed block" `buildWritePlan` makes
  `test_plan_excludes_block0_trailer_unknown` fail (`Expected 4 Was 8` — block 0, trailers 3/7,
  and the `??` block leak into data writes); the real classifier passes.
- `pio run -e cardputer` → SUCCESS, **RAM 26.5% (86 704 B), Flash 74.3% (2 482 675 B)** — just
  +13 KB flash / +0 RAM over the slice-0042 baseline.

**Flash-bloat defect caught and fixed.** The first build came in at Flash 80.9% (+235 KB). Cause:
the parser's first draft used `std::istringstream`/`std::getline`, which drags ~200 KB of C++
iostream/locale machinery into a firmware that otherwise links none of it (confirmed: no other
`src/` file includes `<sstream>`/`<iostream>`). Rewrote `rfid_dump.cpp` with plain char scanning
over `std::string`; behaviour identical (10/10 unchanged), flash fell 222 KB. **Lesson: keep
iostreams out of firmware translation units.**

**On-device:** pending — audit an ordinary MIFARE 1K card with Read Block Data on, restore the
dump onto the same card (UID off) and confirm a data block round-trips; then restore with UID on
and confirm the summary reports block 0 locked (settling the Gen2/CUID question for that card).
Tracked in the LEDGER.
