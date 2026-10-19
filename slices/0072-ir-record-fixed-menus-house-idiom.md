---
id: '0072'
title: "IR record fixed menus adopt the house band, row height, centring and disabled-skip"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

The IR record screen's three hand-rolled fixed menus (`drawMain`, `drawCodeAction`,
`drawTxSourceSelect`), deliberately left out of the slice-0069 list migration because they are
short non-scrolling forms, visibly lag their own migrated sibling list (`drawCodeList`) on the
same screen. The operator found four gaps: the selected-row band never dims when the footer
holds focus, the rows use the roomy `LIST_ITEM_HEIGHT` (20px) instead of the single-line
`LIST_ROW_HEIGHT` (14px), the row text sits high rather than vertically centred, and the main
menu's "Learn (capture)" row stays selectable while greyed out when no IR RX module is present.
Bring all three menus to the house idiom, and make the disabled row unselectable and labelled
with the reason it is greyed.

## Definition of Done

- **Given** the footer hint bar holds input focus on an IR fixed menu
- **When** the menu renders
- **Then** the selected row's band is dimmed to `BG_SECONDARY`, matching the saved-codes list,
  instead of staying the full `BG_SELECTED` highlight

- **Given** any of the three IR fixed menus
- **When** it renders
- **Then** its selectable rows are `LIST_ROW_HEIGHT` (14px) tall with the label vertically
  centred, matching the saved-codes list on the same screen

- **Given** no IR RX module is present (`!captureAvailable_`)
- **When** the operator navigates the main menu
- **Then** the greyed "Learn (capture)" row cannot be selected: the initial selection starts on
  the first enabled row and up/down navigation skips the disabled row, and the row shows an
  inline reason tag (`[no IR RX]`) so the operator sees why it is greyed, since skip-nav means
  it can no longer be selected to surface the old warning toast

- **Given** an IR RX module is present
- **When** the operator navigates the main menu
- **Then** the "Learn (capture)" row is selectable and starts capture as before

## Design

A device-only `paintMenuBand(canvas, rowTop, w, focused)` helper is added to the screen's
anonymous namespace. It paints the full-width selection band at the row top in the house colour,
`BG_SELECTED` when the menu has focus and `BG_SECONDARY` when the footer does, exactly as
`ScrollableList` does. The three menus call it for their selected row with
`!footerHints_.hasFocus()`, then draw their own (menu-specific) coloured label at
`rowTop + LIST_ROW_TEXT_DY` on a `LIST_ROW_HEIGHT` grid. This retires the three divergent
`fillRect(..., BG_SELECTED())`-at-20px copies.

The disabled-skip stays in the screen, not the shared model. `firstEnabledMain()` returns the
first selectable main-menu index given `captureAvailable_` (the Learn row at index 0 is the only
conditionally-disabled one), used to seed `mainSelection_` on entry, and the MAIN navigation
skips the disabled row instead of calling the plain wrapping `listNav`.

**What existing code was considered.** `ListScrollModel`/`ScrollableList` own selection and band
painting for the migrated lists, but neither supports a non-selectable entry: `moveUp`/`moveDown`
wrap over every item. slice-0069 made the same finding for settings' header/separator rows and
kept that skip-nav in the screen on purpose, to avoid adding skip-state and tests to a model with
many consumers for one screen's need. The IR main menu is the second such consumer (a runtime-
disabled row, distinct from settings' structural headers), so per rule of three the skip stays
screen-side and a ledger tracker records that the third consumer should trigger extracting a
shared selectable-predicate into the model. The band helper is kept local (not merged into
`ScrollableList`) for the same reason: only two band-rule copies exist (the component and this
helper), below the rule-of-three threshold, and keeping it local avoids touching a component with
many consumers. `LIST_ROW_TEXT_DY` (the saved-codes list's existing centring offset) is reused
rather than a new literal.

## Verification

- `pio run -e cardputer` SUCCESS and fits. The change is pure rendering plus screen-level nav in
  `IrRecordScreen`, which is device-only (`#ifdef ESP32`) and not host-constructible, so it
  carries no host test, as recorded for the slice-0069 menus; `pio test -e native` stays green
  (no pure logic changed).
- On-device DoD: confirm the three menus show a dimmed band under footer focus, 14px centred
  rows matching the saved-codes list, and that with no IR RX module the Learn row is greyed and
  unselectable while the rest navigate normally.

## As built

Shipped as designed. A device-only `paintMenuBand(canvas, rowTop, w, focused)` helper was added
to the screen's draw block, mirroring `ScrollableList`'s band rule (`BG_SELECTED` focused,
`BG_SECONDARY` when the footer holds focus), and `drawMain`, `drawCodeAction` and
`drawTxSourceSelect` were rebuilt on a `LIST_ROW_HEIGHT` grid with labels centred at
`LIST_ROW_TEXT_DY` and the band drawn via the helper with `!footerHints_.hasFocus()`, retiring the
three divergent 20px `fillRect` copies. The main-menu nav gained a wrap-and-skip loop so it never
rests on the disabled Learn row, `firstEnabledMain()` seeds the initial selection past it, and the
greyed row carries an inline `[no IR RX]` reason tag (the old warning toast is no longer reachable
through selection). The disabled-skip stays screen-side; a ledger tracker records that a third such
consumer should trigger extracting a shared selectable-predicate into `ListScrollModel`.

Measured: `pio run -e cardputer` SUCCESS, Flash 74.6%, 2,492,179 bytes. `pio test -e native`
844/844 (no host-compiled logic changed; the screen is device-only and not host-constructible, so
the change carries no host test, as recorded in Verification). On device the operator confirmed all
DoD scenarios: the three menus dim their band under footer focus and render 14px centred rows
matching the saved-codes list, and with no IR RX module the Learn row is greyed, shows `[no IR RX]`,
and is skipped by navigation while the other rows move normally.
