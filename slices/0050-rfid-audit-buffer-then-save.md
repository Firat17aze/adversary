---
id: '0050'
title: "Buffer the audit dump and name it at save time"
type: slice
status: accepted
date: 2026-10-11
supersedes: []
superseded_by: []
---

## Goal

Deliver the ledger's `[verify] Prompt for a filename when saving the audit's block-data dump`
(operator request 2026-10-10) the way the operator asked for it: name the dump **when saving it
from the summary screen**, after seeing the result — not before the audit even starts. Today the
block dump is **streamed** to `<UID>.nfc` live during the audit, so the filename has to exist up
front; a first cut prompted at audit-start, which the operator found backwards. Move the audit to
**buffer-then-save**: hold the cracked blocks and keys in RAM during the audit, write nothing, and
emit the `.nfc` at save time through the existing `nameInput_` popup (pre-filled with the UID so
Enter keeps the old name). This also collapses a real divergence: the live streamer wrote a proper
Flipper **v4** dump (blocks + keys + UID/ATQA/SAK) to `/adversary/captures/RFID/`, while the
post-hoc `saveToSD()` wrote an inferior **keys-only v3** file to `/adversary/captures/rfid/` (a
different directory by letter-case). One buffered writer replaces both.

## Definition of Done

- **Given** a MIFARE Classic card and the audit launched from the pre-audit prompt
- **When** the operator selects Start
- **Then** the audit runs immediately with **no filename popup**, and nothing is written to SD yet

- **Given** a finished audit sitting on the summary screen
- **When** the operator presses Save (`s`) and accepts the UID-defaulted name in the popup
- **Then** a single Flipper-v4 `.nfc` is written to `/adversary/captures/RFID/<name>.nfc` carrying,
  for every cracked sector, the four block rows (or a named `??` placeholder per unreadable block)
  and Key A / Key B — i.e. the same content the live streamer used to produce

- **Given** the audit read a sector's blocks into the in-RAM buffer
- **When** `getResults()` is inspected after the run
- **Then** that sector's `blockRead[]` flags and `blocks[][]` bytes reflect what was read, with no
  dependency on an open SD file during the audit

## Design

### D1 — Retain block data in the result, drop the live file

`MifareSectorData` gains `uint8_t blocks[4][16]` and `bool blockRead[4]`; the "block data is
streamed, not stored" note is removed. `AuditOptions` drops `dumpPath` (keeps `readData`).
`runDictionaryAttack` no longer opens a `File`: on a cracked sector with `readData`, it runs the
**same** HALT→auth-once-per-sector→read-4-blocks hardware sequence (unchanged, slice-0046/0049) but
copies each block into `sectorData.blocks[block]` / sets `blockRead[block]` instead of printing to a
file. The fail-loud Serial diagnostics (auth-fail, per-block read-fail, no-key-omitted) stay exactly
as slice-0045 left them. The card's UID/SAK are captured into members (`m_uid`, `m_uidSize`,
`m_sak`) at audit start so the save path needs no live card.

Memory: 16 sectors × (64 B blocks + 4 B flags) ≈ **1.1 KB** added to the `m_results` vector on a 1K
card — negligible against the ~205 KB SRAM budget (no-PSRAM invariant), and it is freed with the
vector. This is why buffering is viable here and was not a concern.

### D2 — One writer, fed from the buffer

`saveToSD(filename)` is rewritten to emit the Flipper-v4 format the streamer used (header with UID,
`ATQA: 00 04`, `SAK`, `Mifare Classic type: 1K`, then per-sector `Block N:` rows and `Key A/B:`,
with the named `UNKNOWN_BLOCK_HEX`/`UNKNOWN_KEY_HEX` placeholders), reading from `m_results`. It
writes to `/adversary/captures/RFID/` (matching every other RFID path) — the old `saveToSD`'s
lowercase `/rfid/` and keys-only v3 body are both gone. The block-formatting that lived inline in
`runDictionaryAttack` moves into `saveToSD`; it was one copy in one place, so this is a move, not a
new abstraction.

**Considered and not reused:** `rfid_dump.{h,cpp}` has a `.nfc` *parser* + write-plan builder, but
no serializer (it reads dumps for the writer, it does not write them), so there was nothing to reuse
for emitting the file. The streamer's own formatting block was the thing to preserve, so it was
moved rather than reimplemented.

### D3 — UI: launch directly, prompt on save

`rfid_screen.cpp`: the pre-audit prompt's Start calls `startAuditWithOptions(readData)` directly (no
name, no popup). `startAuditWithOptions` loses its filename argument and builds no path. The
**AUDIT_SUMMARY** `s` handler, which today calls `saveDump("audit_results")` with a hardcoded name
and no prompt, instead shows `nameInput_` pre-filled with `m_auditFilename` (the UID, set at
`promptAudit`); on submit it calls `saveDump(name)` → `m_audit->saveToSD(name)`, with an empty entry
falling back to the UID so a nameless `.nfc` is never written. The popup-forward keeps the
slice-0050 redraw fix (`m_needsRedraw = true` after `nameInput_.handleInput`) so the popup repaints
on every keystroke — without it, typing/backspace/cancel produced no visible change because
`render()` early-returns unless the screen is marked dirty.

## Verification

- **Native (`test_rfid_audit`):**
  - `test_audit_buffers_block_data` — audit an all-factory 1K card with `readData = true` and **no**
    `dumpPath`; assert `getResults()[s].blockRead[b]` is set and `blocks[s][b]` holds the mock's
    bytes for cracked sectors. **Fails before:** `MifareSectorData` has no block storage / the audit
    only wrote to a file, so there is nothing to read back off the struct.
  - `test_saveToSD_writes_full_flipper_dump` — after that audit, `saveToSD("t")`, then read
    `/adversary/captures/RFID/t.nfc` back (the SD mock writes relative to CWD) and assert it carries
    the v4 header, a `Block 0:` row, and a `Key A:` row. **Fails before:** old `saveToSD` wrote
    keys-only v3 to `/rfid/`, so the block row and the path both differ.
  - The migrated read-path diagnostic tests (formerly asserting on the streamed `dumpPath` file) now
    drive the same assertions through `saveToSD`'s output, and the slice-0046/0049 desync + HALT
    regressions (`g_mockReauthNeedsReselect`, `g_mockWupaNeedsHalt`) stay green.
- **Cardputer build:** `pio run -e cardputer` green; RAM/Flash recorded in As built.
- **On-device (Gen1a `23FF9C1E`, non-destructive):**
  1. Audit → **no popup at Start**, audit runs.
  2. On the summary, Save → popup shows the UID as an editable default; backspace/typing/ESC all
     repaint and respond.
  3. Accept → `/adversary/captures/RFID/23FF9C1E.nfc` holds the full blocks + keys (0 unexpected
     `??`), identical in content to the pre-slice streamed dump.

## As built

`MifareSectorData` gained `blocks[4][16]` + `blockRead[4]`; `AuditOptions` dropped `dumpPath`.
`runDictionaryAttack` no longer opens a `File` — it captures `m_uid`/`m_uidSize`/`m_sak` at start and,
on a cracked sector with `readData`, reads the four blocks into the buffer under the one
per-sector auth (the slice-0046/0049 HALT→auth-once→read sequence unchanged). `saveToSD` was
rewritten to emit one Flipper-v4 dump (header + per-sector blocks/keys + named `??` placeholders)
to `/adversary/captures/RFID/`, retiring both the old inline streamer and the lowercase-`/rfid/`
keys-only v3 writer. `rfid_screen` launches the audit directly (no name at Start), prompts at the
summary's Save via the shared `nameInput_` (UID default, empty-falls-back-to-UID), and the
popup-forward now marks the screen dirty so every keystroke repaints. The pre-audit prompt's
misleading "File: <uid>.nfc" line was removed.

Two issues surfaced on-device during the build and were fixed in the same slice:
- **Popup looked frozen** — `render()` early-returns unless `m_needsRedraw`, and the popup-forward
  did not set it, so typing/backspace/ESC produced no visible change (the keys *were* handled). Fixed
  at the shared forward, so the keys-only Save popup benefits too.
- **Dashboard (RESULT) Save offered a dead action** — it calls the same `saveToSD`, which needs audit
  results, so on a bare Read it failed loud with "Save Failed" while the footer still advertised
  "Save". Gated: the Save hint is `hasAuditData()` at every RESULT site (always true at the summary,
  where the data exists) and the handler no-ops when empty, so Save is greyed until an audit runs.

- Native: full suite **811/811** (was 808; +`test_audit_buffers_block_data`,
  +`test_saveToSD_writes_full_flipper_dump`, +`test_saveToSD_fails_when_no_results`; the three
  former `dumpPath` tests migrated to the buffered/`saveToSD` shape). The slice-0046/0049 desync +
  HALT regressions stay green.
- `pio run -e cardputer` **SUCCESS** — RAM 26.5% (86704 B, unchanged), Flash 74.3% (2483527 B,
  ~300 B **smaller** than v0.3.0-alpha's 2483835 B: the streaming path's removal outweighs the
  buffer + gating).
- **On-device CONFIRMED `23FF9C1E`:** audit launched from Start with **no popup**; all 16 sectors
  cracked Key A with **0** `AUTH FAILED`/`READ FAILED`/`omitted` markers and **no reboot** (0 POWERON
  since flash — the earlier mid-audit reboot was key-mashing against the frozen popup, not a crash);
  the summary Save popup showed the UID as an editable default and backspace/typing/ESC all repainted;
  accepting wrote `/adversary/captures/RFID/<name>.nfc`. The dashboard Save was greyed on a bare Read
  and enabled (working) after backing out of a completed audit. All three Definition-of-Done scenarios
  met.
