---
id: '0069'
title: "Selection-list migration wave 3: settings form + ir_record code list"
type: slice
status: accepted
date: 2026-10-18
supersedes: []
superseded_by: []
---

## Goal

Finish the selection-list migration (ledger item 94): route the last two hand-rolled
scrolling lists through the shared `ScrollableList` / `ListScrollModel` component
(slice-0062), so every selection list in the UI shares one tested scroll/band/scrollbar
core. This is the hard wave, deferred through waves 1..2c because the two remaining lists
each break the clean idiom:

- **Settings** is not a homogeneous selection list, it is a heterogeneous config *form*:
  10 non-selectable section `HEADER` rows interleaved in one flat `items_[]`, an EDITING
  mode where up/down adjust a value instead of moving the cursor, and per-type row content
  (NUMBER with a right-aligned value, TOGGLE, INFO, ACTION). Its hand-rolled renderer also
  carries two measured defects: the visible-row count is hardcoded `VISIBLE_ITEMS = 5`
  (dead space below the list, the same class of bug the sniffer banner-overlap fix closed),
  and the hand-rolled scrollbar uses ad-hoc geometry.
- **ir_record** has four "lists" but only one scrolls: the saved-codes list. The other
  three (main menu, code action, TX source) are fixed 2..3 item caption-menus that never
  scroll. It also uses `theme::LIST_ITEM_HEIGHT` (=20, documented for the rfid audit grid
  and static dashboard spacing) for a single-line list, where the theme's own doc reserves
  `theme::LIST_ROW_HEIGHT` (=14) for single-line scrolling rows (slice-0064). That token
  misuse is folded into this leg.

Closing this slice closes ledger item 94.

## Definition of Done

- **Given** the settings screen with more rows than fit on screen
- **When** the operator holds down (`.`) from the first selectable row to the last and past it
- **Then** the cursor skips every `HEADER`/`SEPARATOR` row, wraps from the last selectable
  row back to the first, and the selected row is always scrolled into view (never drawn
  off-screen or under the footer), with the selection band painted by the shared component.

- **Given** a settings `NUMBER` row is selected
- **When** the operator presses Enter and then `;` / `.`
- **Then** the row band turns `WARNING` (the editing cue, distinct from the normal
  `BG_SELECTED` selection band), up/down change the value instead of moving the cursor, and
  Enter / ``` ` ``` returns to browsing with the cursor on the same row.

- **Given** the settings list is taller than the viewport
- **When** it renders
- **Then** the viewport row count is derived from the live list rect (not a hardcoded 5),
  so the list fills the space between the header and the footer, and a single proportional
  scrollbar thumb is drawn by the shared component.

- **Given** the ir_record saved-codes list with more entries than fit
- **When** the operator navigates it and deletes an entry
- **Then** navigation wraps, the selection stays in view, the proportional scrollbar thumb
  appears, and after a delete the selection re-clamps into the shrunken list, all driven by
  the shared model.

- **Given** the ir_record saved-codes list
- **When** it renders
- **Then** each row uses `theme::LIST_ROW_HEIGHT` (14), matching every other migrated
  single-line list, while the three fixed caption-menus are left unchanged.

## Design

Both legs reuse the existing shared component unchanged; **the pure `ListScrollModel` is
not modified** (operator decision: option A over extending the model to skip rows, which
would add state and tests to a component used by 5 other screens for a single consumer, and
over re-scoping settings to item 97, which would leave the two measured defects unfixed).

### What existing code was considered

- `src/ui/components/scrollable_list.h` + `list_scroll_model.h` (slice-0062): reused as-is.
  `render()` derives `visibleRows = h / rowHeight` from the rect, calls
  `model.setViewport()`, paints the band (`BG_SELECTED` focused / `BG_SECONDARY` not), calls
  the per-row callback for content only, and draws the one scrollbar thumb.
- The sniffer (slice-0068) and karma (slice-0067) migrations: followed as the reference
  idiom (model member owns selection+scroll+wrap; content-only callback; viewport from the
  live rect; semantic colours kept in the callback).
- Deliberately **not** reused: a "skippable rows" extension to `ListScrollModel`. Settings'
  header-skip is navigation policy, not scroll state; it stays in the screen (see below), so
  the model keeps its invariant that every index `0..count-1` is selectable from its own
  point of view. The screen simply never asks it to select a header.

### Settings (option A: component paints, screen keeps navigation)

The screen keeps its own `nextSelectable()` header/separator-skip navigation and its EDITING
input routing, and drives the model by index:

- Replace `int selection_` + `int scrollOffset_` with one `ListScrollModel itemList_`.
  `itemList_.selectedIndex()` is the cursor; the model owns the scroll offset.
- `buildSettingsList()` ends with `itemList_.setItemCount(itemCount_)`, keeping the model's
  count in sync on every rebuild (the screen is the owner of the count, per the component
  contract). `show()` then lands the cursor on the first selectable row via
  `itemList_.setSelectedIndex(firstSelectableIndex())` (was the hardcoded `selection_ = 1`).
- Navigation: `;` / `.` call `itemList_.setSelectedIndex(nextSelectable(..., dir))`. The
  model's `setSelectedIndex` clamps and scrolls the selection into view, so the three copies
  of inline scroll-offset math are deleted. `nextSelectable` is unchanged (it walks indices
  and skips headers/separators with wrap).
- `drawItems()` computes the list rect (`listTop = HEADER_HEIGHT + 2`,
  `listH = height - ui::FOOTER_HEIGHT - listTop`) and calls `ScrollableList::render(... ,
  theme::LIST_ROW_HEIGHT, callback)`. This fixes the hardcoded `VISIBLE_ITEMS = 5` (viewport
  now from the rect) and deletes the hand-rolled scrollbar (830-843).
- The per-row callback `drawSettingRow()` draws content only. The component paints the normal
  `BG_SELECTED` band; the callback overpaints `WARNING` only when `isSelected && editing`,
  keeping the editing cue as a *semantic* colour (the same principle that kept the sniffer
  frame-type colours through migration). Right-aligned NUMBER/TOGGLE values are positioned to
  clear the scrollbar column (`screenWidth - VALUE_RIGHT_MARGIN`, margin > scrollbar width).
  Header rows render their `ACCENT` title and never get a band, because the cursor never lands
  on them.
- The selectable predicate `type != HEADER && type != SEPARATOR`, duplicated in three places
  (nav, the `selectableCount_` tally, the footer counter), is extracted to one
  `isSelectableRow(SettingType)` (rule of three). The footer "n/total" counter stays
  screen-level (it excludes headers; the model counts all rows).

Row height drops 18 -> 14 (`theme::LIST_ROW_HEIGHT`), matching every other migrated list;
this is a visible change verified on device (DoD scenario 1/3).

### ir_record (code list only)

Only `CODE_LIST` scrolls, so only it migrates; the three fixed caption-menus keep their
`listNav` helper and `ROW_HEIGHT` (=`LIST_ITEM_HEIGHT`, 20, the roomier caption spacing).

- Replace `int fileSelection_` + `int fileScroll_` with one `ListScrollModel codeList_`.
  All index call sites (`loadActionSignal`, `replaySelected`, `deleteSelected`,
  `drawCodeAction`, `drawConfirm`) read `codeList_.selectedIndex()`.
- Entering the list (`loadCodeList()` then `codeList_.setItemCount(codes_.size())` +
  `setSelectedIndex(0)`) and after a delete (`setItemCount` re-clamps the cursor) replace the
  manual `fileSelection_` / `fileScroll_` resets.
- `CODE_LIST` navigation uses `codeList_.moveUp()` / `moveDown()` (wrap built in); the
  `unusedScroll` dummy that `listNav` forced on it is removed. The three fixed menus still
  pass `unusedScroll` to `listNav`.
- `drawCodeList()` calls `ScrollableList::render(..., theme::LIST_ROW_HEIGHT, callback)` over
  the rect `y = HEADER_HEIGHT .. height - ui::FOOTER_HEIGHT`; the empty-list "No saved codes"
  message is kept as an early return. This adds the previously-absent scrollbar and fixes the
  token misuse.

## Verification

- Host unit tests: `pio test -e native` must stay green (840 tests). The migration adds no
  new host-testable logic (`ListScrollModel`'s wrap/scroll/thumb math is already covered by
  the slice-0062 suite, `test/test_list_scroll_model.cpp`); this slice only wires two more
  screens onto that tested core, so the guard is that the existing suite still passes and the
  device build links.
- Device build: `pio run -e cardputer -j 4` must compile and link (the `ScrollableList`
  template is instantiated for both screens).
- On-device acceptance: the five Definition-of-Done scenarios above, confirmed by the
  operator on the flashed firmware (settings header-skip + wrap + scroll-into-view; settings
  EDITING band + value adjust; settings viewport-from-rect + scrollbar; ir_record code-list
  wrap + delete re-clamp + scrollbar; ir_record row height). Screen rendering has no host
  harness (screens are not host-constructible), so device confirmation is the proof for the
  rendering scenarios, as in waves 1..2c.

## As built

Shipped as designed (option A), both legs, confirmed on device 2026-10-19. The pure
`ListScrollModel` was not modified.

**Settings** (`settings_screen.{h,cpp}`): `selection_` + `scrollOffset_` + the hardcoded
`VISIBLE_ITEMS = 5` and `ROW_HEIGHT = 18` collapsed into one `ListScrollModel itemList_`. The
screen keeps its `nextSelectable()` header/separator-skip navigation and its EDITING input
routing and drives the model by index; the three copies of inline scroll-offset math are gone
(`setSelectedIndex` scrolls into view). `drawItems()` now calls `ScrollableList::render` with
the viewport from the live rect (`canvas.height() - ui::FOOTER_HEIGHT - listTop`), deleting the
hand-rolled scrollbar; the per-row content moved to `drawSettingRow()`, which overpaints the
`WARNING` editing band over the component's `BG_SELECTED` band (semantic cue kept). Right-aligned
NUMBER/TOGGLE values are positioned at `screenWidth - VALUE_RIGHT_MARGIN` so they clear the
scrollbar column. Row height dropped 18 -> 14 (`theme::LIST_ROW_HEIGHT`). The selectable predicate
(three copies) was extracted to `isSelectableRow()`, and the NUMBER value formatting (including the
toast-position / karma-speed / wardriving-mode enum-as-word special cases) to `formatNumberValue()`.
The focused flag is `!footerHints_.hasFocus()`, matching karma/sniffer.

**ir_record** (`ir_record_screen.{h,cpp}`): `fileSelection_` + `fileScroll_` collapsed into one
`ListScrollModel codeList_`; `drawCodeList()` routed through `ScrollableList` at
`theme::LIST_ROW_HEIGHT` (14), adding the previously-absent scrollbar and fixing the
`LIST_ITEM_HEIGHT` (20) token misuse for a single-line scrolling list. Navigation uses the model's
built-in wrap; deletion re-clamps via `setItemCount`. The three fixed caption-menus (main, code
action, TX source) were left on `listNav` with `ROW_HEIGHT = LIST_ITEM_HEIGHT` unchanged.

**Incidental fix found during on-device test (DoD scenario 1):** the leading section header
("Karma Configuration") was scrolled off the top on entry. Root cause: `show()` called
`setSelectedIndex(firstSelectableIndex())` (row 1) before the first render set the viewport, so
the model's default `visibleRows_ = 1` made `clampOffsetToSelection` over-scroll to offset 1, and
a later render could not recover (it only scrolls down to reveal the cursor, never back up). Fixed
by calling `itemList_.setViewport(itemCount_)` (assume all rows fit) before the initial
`setSelectedIndex`, which pins the offset at 0; the first render then sets the true viewport over
the correct offset. This fix is screen-level (`show()` is not host-constructible) and the model was
unchanged, so it carries no host regression test (fail-before/pass-after is unmeetable at the model
level); it is covered by the on-device header-visible check, as with every other rendering scenario
in waves 1..2c.

All five Definition-of-Done scenarios confirmed on device: settings header-skip + wrap +
scroll-into-view with the header visible; settings EDITING `WARNING` band + value adjust; settings
viewport-from-rect (list fills to the footer) + scrollbar clear of values; ir_record code-list wrap
+ delete re-clamp + scrollbar; ir_record 14px rows. Measured: native **840/840**; cardputer links,
Flash **74.6%** (2,492,115 bytes, +364 vs 449b8c0).

This closes ledger item 94 (the selection-list migration is complete: every genuine selection list
in the UI now shares the slice-0062 component). Deliberately left for other ledger items: the three
ir_record fixed caption-menus (never scroll, migrating them would be over-abstraction), and settings
as a config *form* remains a candidate for item 97 (config-form unification) should that work want
a shared form abstraction later.
