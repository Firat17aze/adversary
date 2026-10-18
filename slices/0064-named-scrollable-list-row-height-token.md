---
id: '0064'
title: "Named ScrollableList row-height token"
type: slice
status: accepted
date: 2026-10-18
supersedes: []
superseded_by: []
---

## Goal

Fold in the deferred ledger item **"Named `ScrollableList` row-height tokens"** (split out
of the appearance capstone slice-0063 by operator decision 2026-10-18: colour first, height
on its own measured slice).

After slice-0063 every selection list shares one component and one selected-row appearance,
but the **row height** is still a per-screen literal spelled under five different names. The
value `14` (the standard single-line row) is defined independently at eight call sites, with
two near-neighbours that look like unintentional drift (`15` in ble_spam, `16` in whitelist).
That is a rule-of-three DRY defect: one concept, eight definitions.

This slice introduces a single named token for the standard single-line list row, routes the
eight single-line sites through it, and normalises the two drift values onto it. It does
**not** try to unify the two-line rows (captures `20`, ble_scanner `22`, scanner `24`) or the
roomier rfid grid (`20`): those are content-tuned, each used once, and carry no duplication to
remove (see Design).

## Definition of Done

- **Given** the theme layer
- **When** a developer needs the standard single-line list-row height
- **Then** there is exactly one named token, `theme::LIST_ROW_HEIGHT`, to use, and no
  single-line list call site spells the height as a bare `14`/`15`/`16` literal.

- **Given** the eight list screens that already rendered single-line rows at 14px
  (beacon_spam config, probe_flood config, deauth config, wardriving log, handshake history,
  radio's three lists)
- **When** they render after the change
- **Then** the output is pixel-identical to before (pure refactor: the token equals the value
  those sites already used).

- **Given** ble_spam config (was 15px) and whitelist (was 16px)
- **When** they render after the change
- **Then** their rows are 14px, every option/entry is still fully visible with no clipping,
  and the visible-row count is unchanged-or-higher (normalising downward only tightens
  spacing, it never drops a row), confirmed on device.

- **Given** the two-line lists (captures, ble_scanner, scanner) and the rfid audit/write-pick
  grid
- **When** they render after the change
- **Then** their heights are untouched.

- **Given** the repository checks
- **When** the native suite and the cardputer build run
- **Then** native stays green (833/833) and the cardputer firmware compiles.

## Design

### Measured input

Row heights passed to `ScrollableList::render` across the 15 migrated sites (survey
2026-10-18):

| Height | Rows | Sites | Spelled as |
|--------|------|-------|-----------|
| **14** | single-line | beacon_spam, probe_flood, deauth (all three config lists), wardriving, handshake, radio ×3 | `lineHeight` / `itemHeight` / `LINE_HEIGHT` (5 distinct names) |
| 15 | single-line | ble_spam config | `LINE_HEIGHT` (ble_spam.h) |
| 16 | single-line | whitelist | `ROW_HEIGHT` (whitelist.h) |
| 20 | mixed | captures (two-line), rfid ×2 (one-line grid) | `ROW_HEIGHT` (captures) / `theme::LIST_ITEM_HEIGHT` (rfid) |
| 22 | two-line | ble_scanner | `itemHeight` |
| 24 | two-line | scanner | `ENTRY_HEIGHT` |

The DRY defect is the **14 cluster**: one concept, eight definitions, five names. 15 and 16 are
one-off near-14 values with no deliberate reason recorded, i.e. drift. The two-line values
(20/22/24) are three *different* content-tuned heights, each used once, so they are not a
rule-of-three case: a shared "two-line" token would have to pick one value and would change
the visible-row count on the other two screens for no DRY payoff. `theme::LIST_ITEM_HEIGHT`
(=20) is also overloaded (rfid list rows **and** static dashboard spacing in modules/ir_record),
but disentangling that name touches non-list code and belongs with the ir_record migration,
not here.

**Correction to the prior ledger claim:** that item proposed reconciling the two-line spread
(20/22/24) "to one value" as well. The measurement says no: those are single-use, content-fit
heights with no duplication, so forcing one value is unification for its own sake at a visual
cost. This slice deliberately scopes two-line out. (Operator confirmed 2026-10-18: tokens now,
normalise only the 15/16 one-line outliers.)

### The token

Add to `src/ui/theme_manager.h` beside the existing component dimensions (line 418):

```cpp
// The standard single-line scrolling-list row. Distinct from LIST_ITEM_HEIGHT (=20),
// which is the roomier rfid audit-grid row and static dashboard spacing, not this.
constexpr uint8_t LIST_ROW_HEIGHT = 14;
```

### Per-site change

Two shapes, by whether the local height constant is reused for surrounding layout:

- **Render-only constant** (the height feeds nothing but the `render` rowHeight arg): point
  the constant at the token, which also normalises 15/16 → 14.
  - beacon_spam `lineHeight` (drawConfig), wardriving `itemHeight`, handshake `itemHeight`:
    `= theme::LIST_ROW_HEIGHT`.
  - ble_spam `LINE_HEIGHT` (15→14), whitelist `ROW_HEIGHT` (16→14): class constant
    `= theme::LIST_ROW_HEIGHT` (confirm the header pulls in theme_manager.h via ui/theme.h).
- **Reused layout constant** (the name also drives `y += ...` math in the same function or
  across the screen): keep the name, derive its value from the token so the layout is
  byte-identical and the literal is single-sourced.
  - probe_flood `lineHeight` (drawConfig, also `y += lineHeight`), deauth `lineHeight`
    (drawConfig, also `y += lineHeight`): local `= theme::LIST_ROW_HEIGHT`.
  - radio `LINE_HEIGHT` (radio.h, used in 30+ layout sites across the screen): class constant
    `= theme::LIST_ROW_HEIGHT`. This is the load-bearing one: radio's whole screen spaces on
    `LINE_HEIGHT`, so it must stay 14; deriving it from the token removes the duplicate literal
    without moving a pixel.

Untouched by rule (no duplication to remove): captures `ROW_HEIGHT`=20, ble_scanner
`itemHeight`=22, scanner `ENTRY_HEIGHT`=24, rfid's `theme::LIST_ITEM_HEIGHT`=20.

### Existing code considered

Searched the whole `src/ui` tree for the height literals and for `LIST_ITEM_HEIGHT`. Reused
the existing `theme::` component-dimension block (where `LIST_ITEM_HEIGHT`, `BUTTON_HEIGHT`,
`MENU_ITEM_HEIGHT` already live) rather than inventing a new home. Deliberately did **not**
reuse `LIST_ITEM_HEIGHT` for the new token: its value (20) is wrong for the single-line row,
and its meaning is already overloaded. Deliberately did **not** introduce a two-line token:
no rule-of-three case (see above).

### Normalisation safety

Normalising is downward only (15→14, 16→14), which tightens row spacing and can only keep or
*increase* the visible-row count; it cannot drop a row or clip content (the hazard the ledger
flagged was the opposite direction, forcing one-line rows *up* to 20). ble_spam config has a
fixed 4-row list that fits comfortably at either height; whitelist gains at most one extra
visible entry. Both are confirmed on device per the DoD.

## Verification

This is a display-constant refactor: the behaviour is a pixel height, which no pure-logic unit
test can assert, and this repo has no rendering-verification harness wired up (the slice
template's `scripts/<slice>-verify.mjs` + package.json wiring does not exist here). Proof is
therefore:

- **Native suite green** — `pio test -e native` stays at 833/833 (guards that nothing in the
  shared `ListScrollModel` / component contract regressed; the token is display-only so no new
  unit test is warranted, and a test asserting a constant equals 14 would test the
  implementation, not behaviour).
- **Cardputer build** — `pio run -e cardputer -j 4` compiles (the `-j 4` is a host-RAM
  constraint, not a code one; default `-j 12` OOM-kills on this ~5 GiB host).
- **On-device visual** — confirm ble_spam config and whitelist (the two normalised screens)
  render with no clipping and the expected row count, and spot-check one unchanged 14-site
  (e.g. deauth config) for pixel parity.

## As built

Shipped as designed. `theme::LIST_ROW_HEIGHT = 14` added beside the component dimensions in
`theme_manager.h`, and the eight single-line sites routed onto it:

- **Keep-and-derive** (constant reused for layout math, so the name stays and only the literal
  is single-sourced): radio `LINE_HEIGHT` (radio.h, drives 30+ `y += ...` sites), probe_flood
  and deauth config `lineHeight`, plus beacon_spam/wardriving/handshake locals.
- **Inlined at the call site**: ble_spam, because its header did not include the theme layer
  and the constant was used only once; the class `LINE_HEIGHT` constant was removed and the
  call now passes `theme::LIST_ROW_HEIGHT` directly (`ble_spam_screen.cpp` includes `ui/theme.h`).
- **Normalised**: ble_spam 15→14 and whitelist 16→14 (whitelist via its header constant, which
  already pulled in the theme layer).

The two-line heights (captures 20, ble_scanner 22, scanner 24) and rfid's `LIST_ITEM_HEIGHT`
(20) were left untouched, as designed.

**Deviation — whitelist row-text centering.** Shrinking whitelist 16→14 surfaced that it drew
its text at `rowY + 1` (never actually centred, just less visible in the taller band). On-device
review flagged it as top-aligned. Fixed in two steps against operator feedback: first to `+2`
(the convention the other single-line lists use), still read ~2px high, then to `rowY + 4`,
which the operator confirmed as centred. Root cause measured: the 6x8 default font's ink
occupies the top ~7px of its 8px cell, so the band's geometric centre is not the visual centre;
`+4` is the true visual centre of a single-line glyph in the 14px band. The other single-line
lists stay at `+2` (~1.5px high by the same effect, operator judged acceptable), leaving a
+4-vs-+2 split; the shared-centring cleanup is logged to the ledger as a follow-up.

**Verification run:** native `pio test -e native` 833/833; `pio run -e cardputer -j 4` SUCCESS
(Flash 74.5%, flat vs before); flashed to /dev/ttyACM0 and the two normalised screens
(ble_spam config, whitelist) plus a deauth-config spot-check confirmed on device.
