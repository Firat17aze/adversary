---
id: '0074'
title: "Shared vertical-centring offset for single-line list rows"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

Every migrated single-line list positions its row text with a hand-picked vertical offset, and
the offsets disagree: most screens draw at `rowY + 2`, `whitelist` at `rowY + 4`, and
`ir_record` at `rowTop + LIST_ROW_TEXT_DY` where that is a file-local `= 3`. The three values
are three different answers to one question (where does a line of the default font sit, centred,
in a `theme::LIST_ROW_HEIGHT` band), so the same list row reads at a different height depending on
which screen drew it. This closes the open ledger follow-up that recorded the `+4`-vs-`+2` split
left by slice-0064: it replaces the scattered literal with one shared offset derived from the band
height and the font's glyph height, applied to every single-line row, so the answer lives in one
place and self-corrects if either dimension moves.

The measured basis: the default 6x8 GFX font occupies an 8px cell and `setCursor` sets the cell
top, so centring one line in the 14px band is `(14 - 8) / 2 = rowY + 3`. The cell's ink is
top-weighted (a 5x7 glyph with a 1px bottom gap), so cell-centring reads a touch high rather than
low. slice-0064 had corrected `whitelist` to the ink-centred `+4` and left the rest at `+2`; on
device 2026-10-19 the operator judged `+4` too low and chose `+3` for the unified value, revising
that slice-0064 call. Net per-site effect: the `+2` sites move down 1px, `whitelist` moves up 1px
(`+4` to `+3`), and `ir_record` (already `+3`) keeps its position and loses its duplicate constant.

## Definition of Done

- **Given** any migrated single-line list screen (deauth/beacon_spam/probe_flood config, radio x3,
  wardriving, handshake, ble_spam, badusb, ble_apple_attack, ble_bad_ble x2, karma, server_menu,
  whitelist, ir_record menus)
- **When** it renders a row of text in its `theme::LIST_ROW_HEIGHT` band
- **Then** the text baseline is `rowY + theme::LIST_ROW_TEXT_DY`, one shared offset, so every
  single-line row across every screen sits at the same height within its band

- **Given** the shared `theme::LIST_ROW_TEXT_DY`
- **When** it is evaluated
- **Then** it equals 3 for the current 14px band and 8px font cell, and is defined as a `constexpr`
  expression over `LIST_ROW_HEIGHT` and the font cell height (not a bare literal), so changing
  either dimension re-centres every row without touching a call site

- **Given** the multi-line rows in `captures` (20px band, two text lines plus a status dot) and
  `saved_networks` (28px band, two text lines)
- **When** the change lands
- **Then** the multi-line rows are untouched (their per-line offsets are not a single-line centre
  and are out of scope)

- **Given** `ir_record`, which drew its menus at a file-local `LIST_ROW_TEXT_DY = 3`
- **When** the change lands
- **Then** the file-local constant is removed in favour of the shared `theme::LIST_ROW_TEXT_DY` of
  the same value, so its menu text keeps its position and no second definition of the offset remains

## Design

`theme::LIST_ROW_HEIGHT` (=14) already names the single-line band; what was missing is a companion
token for where the text sits inside it. Add `theme::LIST_ROW_TEXT_DY` next to it, derived rather
than literal so the ledger's "stays correct if either changes" requirement holds:

```cpp
// The default 6x8 GFX font occupies an 8px cell and setCursor sets the cell top, so centring one
// line in a row band is (LIST_ROW_HEIGHT - FONT_CELL_HEIGHT) / 2 = 3 for the 14px band. The cell's
// ink is top-weighted (a 5x7 glyph with a 1px bottom gap), so cell-centring reads a touch high
// rather than low; on device 2026-10-19 the operator preferred this to the ink-centred +4 (which
// sat too low, revising the slice-0064 whitelist value). A bare +2 reads ~2px high. Derived from
// both dimensions so a change to either re-centres every single-line row with no call-site edit.
constexpr uint8_t FONT_CELL_HEIGHT = 8;
constexpr int16_t LIST_ROW_TEXT_DY = (LIST_ROW_HEIGHT - FONT_CELL_HEIGHT) / 2;
```

This centres the full 8px font cell in the band, an exact halving with no rounding term: `(14 - 8)
/ 2 = 3`. Because the cell's ink sits in its top 7px, the line reads marginally high of dead
centre, which on device 2026-10-19 the operator preferred to the ink-centred `+4`. Every
single-line row site then draws at `rowY + theme::LIST_ROW_TEXT_DY`.

**What existing code was considered.** `theme::LIST_ROW_HEIGHT` is the sibling token and the new
one sits beside it so the pair reads as band-plus-baseline. `whitelist`'s `rowY + 4` (slice-0064,
on-device) was the previous reference, but on device 2026-10-19 the operator judged it too low, so
the unified value is the cell-centred `+3` and `whitelist` moves up 1px with everything else; its
now-redundant local comment collapses to the shared token. `ir_record`'s file-local
`LIST_ROW_TEXT_DY = 3` is the only other named offset and already equals the shared value; it is
removed, not kept alongside the new token, because two definitions of one baseline is the
duplication this slice exists to end. The
multi-line rows were considered and deliberately **excluded**: `captures` (`ROW_HEIGHT = 20`, two
text lines at `+2`/`+11` around a status dot at `+6`) and `saved_networks` (`ENTRY_HEIGHT = 28`,
two text lines at `+4`/`+14`) do not centre a single line in a 14px band, so their offsets are a
different layout problem and forcing them onto this constant would misplace their second line. The
non-list `lineHeight = 16/14/20` draws in beacon_spam/deauth/probe_flood/karma/ble_bad_ble (status
and confirmation panels, not the config `ScrollableList`) are likewise left alone.

The call-site replacement across the 14 single-line screens is a mechanical find-and-replace over
an enumerated list of exact sites (no per-site judgement once the in-scope set is fixed), so it was
delegated to Haiku 4.5 with the site table; the constant, the scope boundary, and the review are
owned here.

## Verification

- `pio run -e cardputer` SUCCESS and fits: the change is pure render-offset wiring (a new
  `constexpr` plus `setCursor` baseline edits) behind the device UI, not host-constructible, so it
  carries no host test, as recorded for the slice-0069/0072/0073 UI work. `pio test -e native`
  stays green (844/844) because no pure/host-compiled logic changed.
- On-device DoD: across the enumerated screens the single-line row text reads centred in its band
  (the `+2` sites move down 1px, `whitelist` up 1px, `ir_record` unchanged); `captures` and
  `saved_networks` two-line rows are unchanged. This is the one criterion the host cannot assert (it
  is glyph placement on the panel), so it is verified by eye on the Cardputer.

## As built

Shipped as one shared token, `theme::LIST_ROW_TEXT_DY`, added beside `LIST_ROW_HEIGHT` in
`theme_manager.h` and derived `(LIST_ROW_HEIGHT - FONT_CELL_HEIGHT) / 2`, which is 3 for the current
14px band and 8px font cell. Every single-line row across the 14 screens now draws at `rowY +
theme::LIST_ROW_TEXT_DY`: the 27 `+ 2` call sites in the 12 straightforward screens were swapped
mechanically (delegated to Haiku 4.5 against an exact site table), `whitelist`'s `+ 4` collapsed to
the token, and `ir_record`'s file-local `LIST_ROW_TEXT_DY = 3` was removed in favour of the shared
one (same value), ending the duplicate definition.

The value was not the one first built. The initial cut used the ink-centred `+ 4` (slice-0064's
on-device-confirmed value, expressed as `(LIST_ROW_HEIGHT - 7 + 1) / 2`); on device 2026-10-19 the
operator judged `+ 4` too low and chose the cell-centred `+ 3`, so the constant was retuned to
`(LIST_ROW_HEIGHT - FONT_CELL_HEIGHT) / 2` with `FONT_CELL_HEIGHT = 8`, an exact halving with no
rounding term. That revises the slice-0064 whitelist value: the `+ 2` sites moved down 1px,
`whitelist` moved up 1px, and `ir_record` kept its position. The multi-line rows stayed out of
scope as designed: `captures` (`ROW_HEIGHT = 20`, two lines plus a status dot) and `saved_networks`
(`ENTRY_HEIGHT = 28`, two lines) were untouched, as were the hand-drawn `lineHeight = 16/14/20`
config panels (ble_bad_ble config form, radio status line) the subagent surfaced and correctly
left alone.

Measured: `pio run -e cardputer` SUCCESS, Flash 74.6%, 2,492,475 bytes (byte-identical to the
pre-slice `87ab435`, since swapping a compile-time immediate for a folded `constexpr` adds no code).
`pio test -e native` 844/844 (no host-compiled logic changed). `lint-docs.mjs` 0/0. On device
2026-10-19 the operator confirmed the unified `+ 3` reads centred across the enumerated lists, with
`whitelist` and the two-line rows correct.
