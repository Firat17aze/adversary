---
id: '0066'
title: "Selection-list migration wave 2a: the BLE BadBLE lists"
type: slice
status: accepted
date: 2026-10-18
supersedes: []
superseded_by: []
---

## Goal

Continue the deferred ledger item **"Finish the selection-list migration: ~10 screens"**. Wave 1
(slice-0065) routed the four simplest single lists through the shared component. A survey
(2026-10-18) of the screens the ledger grouped as "wave 2" found that the "band-idiom reconcile"
label was optimistic: the four remaining screens hold **seven** lists of **three distinct shapes**,
not one uniform reconcile:

- **Genuine scrolling selection lists** that fit `ScrollableList` (1 row = 1 index = 1 band):
  ble_bad_ble action-menu, ble_bad_ble ducky picker, karma clients, sniffer packet log.
- **Scroll-only logs** with no selection index (karma probes, evil_twin credentials): a scroll
  window over 2-line rows with bespoke `^`/`v` carets and no band. The slice-0065 scope rule
  (*only genuine scrolling **selection** lists migrate*) excludes these; unifying their carets into
  the proportional thumb needs a **no-selection mode** on the component, which is a new capability,
  not a routing change. Ledgered as its own decision.
- **Dead/broken**: evil_twin clients list declares a scroll var it never reads, iterates a fixed
  `clients[8]` stack array decoupled from the real client count, and silently truncates. Ledgered
  as a bug (boyscout), not migrated.

This wave (**2a**) takes the two lowest-risk genuine selection lists, both in
`ble_bad_ble_screen`, in one coherent commit:

- **action-menu** (CONNECTED state): the 12-item OS-command list. Already implements almost exactly
  the target focus-aware band + a proportional scrollbar, so it is nearly a drop-in.
- **ducky picker** (SCRIPT_SELECT state): the SD-backed DuckyScript file list.

Karma clients (wave 2b) and sniffer packet log (wave 2c) follow as separate risk-ordered slices.

## Definition of Done

- **Given** the two ble_bad_ble lists (action-menu, ducky picker)
- **When** each renders after the change
- **Then** the selected row is drawn by `ScrollableList::render` (the component paints the band and
  the one proportional thumb), the per-row callback draws content only and never fills its own band
  or draws its own scrollbar, and neither list hand-rolls a selection/scroll loop.

- **Given** the single-line rows of both lists
- **When** they render
- **Then** their row height is `theme::LIST_ROW_HEIGHT` (14), replacing the bespoke `18` each used,
  and every row's text is fully visible with no clipping.

- **Given** the selection sits on the last item and the operator presses Down (or first item + Up)
- **When** the input is handled
- **Then** the selection wraps to the other end on both lists, matching every already-migrated list
  (a deliberate behaviour change: both clamped before; see Design).

- **Given** the action-menu's item count
- **When** navigation clamps/wraps are evaluated
- **Then** the count comes from a named constant fed to the model, not the bare literal `11`/`12`
  the old clamp used.

- **Given** the DuckyScript entry is chosen from any of the three target-OS action switches
- **When** the screen enters SCRIPT_SELECT
- **Then** the identical load-and-reset sequence runs from **one** place (the three copies are
  folded into a single helper), and the ducky list's selection resets to the top through the model.

- **Given** the repository checks
- **When** the native suite and the cardputer build run
- **Then** native stays green (833/833) and the cardputer firmware compiles.

## Design

### Measured starting state (survey 2026-10-18)

| List | selection+scroll vars | band idiom today | row height | scrollbar today | wrap today | focus wiring |
|------|------------------------|------------------|------------|-----------------|------------|--------------|
| action-menu (CONNECTED) | `int m_actionSelection`, `int m_scrollOffset` (cpp:285,279) | `BG_SELECTED`+`TEXT_INVERSE` focused / `BG_SECONDARY`+`TEXT_PRIMARY` footer-focus (cpp:286-296) | `ROW_HEIGHT=18`, `VISIBLE_ROWS=5` (cpp:276-277) | bespoke proportional thumb (cpp:304-320) | **clamps**, hardcoded max index `11` (cpp:671-690) | dispatch wired + render checks `hasFocus()` (cpp:288,662) |
| ducky picker (SCRIPT_SELECT) | `int m_bleScriptSel`, `int m_bleScriptOffset` (cpp:735,730) | `BG_SELECTED`+`TEXT_INVERSE`, **no** footer-focus branch (cpp:736-741) | `ROW_H=18`, `VISIBLE=5` (cpp:725-726) | bespoke proportional thumb (cpp:747-757) | **clamps** (cpp:617-630) | dedicated handleInput block **bypasses** `footerHints_` entirely (cpp:613-641) |

The 12-item action list is built fresh each frame from three OS-specific `const char*[12]` arrays
plus a dynamic "Layout: [US/ES]" row; the ducky list is populated from an SD directory scan
(`loadBleScripts`) and has an empty-state early return.

### The component contract being adopted

`ScrollableList::render(canvas, model, x, y, w, h, rowHeight, drawRow, focused)` paints the
selected-row band (`BG_SELECTED`/`BG_SECONDARY` by `focused`), derives `visibleRows = h/rowHeight`,
feeds that viewport to the model, and draws the one proportional thumb when `model.isScrollable()`.
The screen keeps a `ListScrollModel` per list, feeds `setItemCount(...)` each frame, drives it with
`moveUp()`/`moveDown()`, and reads `selectedIndex()` wherever it needs the current row. The callback
draws **content only**.

### Per-list changes

- **action-menu**: replace `m_actionSelection`+`m_scrollOffset` with `ListScrollModel m_actionList`.
  `drawConnected` sets `m_actionList.setItemCount(ACTION_ITEM_COUNT)` and renders the list through
  the component at `theme::LIST_ROW_HEIGHT`, with the list rect spanning from the status bar
  (`listTop = 22`) down to the footer (`listH = SCREEN_HEIGHT - FOOTER_HEIGHT - listTop`). The
  callback prints `currentActions[idx]` content-coloured (no inverse). Focus is passed as
  `!footerHints_.hasFocus()`, preserving the one list that already dims correctly on footer focus.
  `handleInput` Up/Down become `setItemCount + moveUp/moveDown`; `injectCommand`'s three
  `switch (m_actionSelection)` read `m_actionList.selectedIndex()`.
- **ducky picker**: replace `m_bleScriptSel`+`m_bleScriptOffset` with `ListScrollModel
  m_bleScriptList`. `drawScriptSelect` keeps the empty-state early return, then renders through the
  component at `theme::LIST_ROW_HEIGHT`. `handleInput`'s SCRIPT_SELECT block becomes `setItemCount +
  moveUp/moveDown`; Enter runs `m_bleScriptList.selectedIndex()`.
- **ACTION_ITEM_COUNT**: the bare `12`/`11` of the action list becomes a named
  `static constexpr int ACTION_ITEM_COUNT = 12;` (no magic value), fed to the model.
- **enterScriptSelect()**: the DuckyScript-entry block (`loadBleScripts()`, reset selection, enter
  SCRIPT_SELECT, set hints, redraw) is duplicated verbatim in all three OS switches of
  `injectCommand` (cpp:421-428, 460-467, 504-511). The migration edits all three (the selection
  reset must now go through the model), so by the rule of three they are folded into one private
  helper, with the reset expressed as `m_bleScriptList.setItemCount(size)` +
  `setSelectedIndex(0)`.

### Deliberate behaviour and visual changes (to confirm on device)

1. **Text no longer inverts on selection.** Both lists drew selected-row text `TEXT_INVERSE`; the
   house idiom (slice-0063) is content-coloured text on the band. They become content-coloured,
   matching every migrated list.
2. **Row height 18 to 14** on both single-line lists. The component derives the visible-row count
   from the rect, so more rows fit (≈6 vs 5); nothing clips.
3. **Wrap replaces clamp** on both lists, matching every already-migrated list (operator accepted
   this for consistency in wave 1).
4. **The ducky picker gains no footer-focus dimming** and keeps its always-focused band: its
   SCRIPT_SELECT handleInput block never wired `footerHints_.handleInputWithDispatch`, so its footer
   is decorative. This is the exact pre-existing gap already ledgered after wave 1 (badusb /
   mouse_jiggler / server_status). Wiring it is deferred to that slice so this stays a pure routing
   change; `focused` is passed as `true` (matching current always-full-band behaviour). The
   action-menu, which *does* wire dispatch, keeps its correct footer-focus dimming.

### Centering offset

Single-line row text is drawn at `rowTopY + 2`, matching the majority of migrated lists. The
`+2`-vs-`+4` reconciliation stays deferred to the ledger's shared-centering item; this wave does not
pre-empt it.

### Existing code considered

Reused the shared `ScrollableList`/`ListScrollModel` (slice-0062) and `theme::LIST_ROW_HEIGHT`
(slice-0064) as wave 1 did. Two models are kept (one per list) rather than sharing one: the lists
live in different states with different item counts and independent selections, so a shared model
would clobber selection across state changes for no gain. Deliberately did **not** migrate the IDLE
config menu in the same file (`drawIdle`, 4 fixed rows Name/Type/Target/START): it never scrolls, so
the scope rule leaves it hand-rolled. Deliberately did **not** migrate the scroll-only logs (karma
probes, evil_twin credentials) or the broken evil_twin clients list: both are ledgered (a
scroll-only component mode, and a bug, respectively). No new abstraction introduced beyond the
`enterScriptSelect` helper, which removes existing triplicated code.

## Verification

Same shape as slice-0064/0065: a display-path refactor whose behaviour is pixel layout, which no
pure-logic unit test can assert, and the repo has no rendering-verification harness. The
`ListScrollModel` selection/scroll/wrap math is already unit-tested and is **unchanged** by this
slice, so the native suite guards that the shared contract did not regress; no new unit test is
warranted (a test asserting a screen calls `render` would test the implementation, not behaviour).

- **Native suite** — `pio test -e native` stays 833/833.
- **Cardputer build** — `pio run -e cardputer -j 4` compiles (`-j 4` is the host-RAM cap; default
  `-j 12` OOM-kills this ~5 GiB host).
- **On-device visual** — flash and confirm: the action-menu renders through the component with the
  unified band + thumb, dims when the footer holds focus, wraps at both ends, and every OS's 12
  commands still fire on the correct item after the index now comes from the model; the ducky picker
  (populate >6 scripts on SD) shows the unified band + thumb, wraps, and Run launches the selected
  script; no clipping at the new row height.

## As built

Shipped as designed. Both ble_bad_ble lists now keep a `ListScrollModel` and render through
`ScrollableList::render`, with the per-row callback drawing content only:

- **action-menu** (CONNECTED) — `m_actionSelection`+`m_scrollOffset` replaced by `m_actionList`;
  row height `theme::LIST_ROW_HEIGHT` (was `18`); the bespoke thumb replaced by the unified one; the
  list rect now spans status-bar-to-footer so the component derives the visible count (≈6 rows vs the
  old hardcoded 5); selected-row text no longer inverts; Up/Down wrap; footer-focus dimming
  preserved (`focused = !footerHints_.hasFocus()`). `injectCommand`'s three `switch` statements read
  `m_actionList.selectedIndex()`.
- **ducky picker** (SCRIPT_SELECT) — `m_bleScriptSel`+`m_bleScriptOffset` replaced by
  `m_bleScriptList`; row height `theme::LIST_ROW_HEIGHT` (was `18`); bespoke thumb replaced by the
  unified one; empty-state early return kept; selected-row text no longer inverts; Up/Down wrap. Its
  SCRIPT_SELECT handleInput still does not wire footer-focus dispatch, so `focused` is passed `true`
  (always-focused band, matching prior behaviour); wiring it stays the ledgered follow-up.
- **ACTION_ITEM_COUNT** — the bare `12`/`11` of the action list is now a named
  `static constexpr int ACTION_ITEM_COUNT = 12`, fed to the model.
- **enterScriptSelect()** — the DuckyScript-entry block duplicated verbatim in all three OS switches
  of `injectCommand` is folded into one private helper, with the ducky list reset expressed through
  the model (`setItemCount` + `setSelectedIndex(0)`).

The IDLE config menu (`drawIdle`, 4 fixed rows) was deliberately left hand-rolled per the scope
rule; its pre-slice-0063 band now visibly lags the unified CONNECTED screen, logged as its own
restyle follow-up. The scroll-only logs (karma probes, evil_twin creds) and the broken evil_twin
clients list were scope-excluded and ledgered (a scroll-only component mode, and a bug).

Single-line row text is at `rowTopY + 2` (majority convention); the `+2`-vs-`+4` reconciliation stays
deferred to the ledger's shared-centering item.

**Verification run:** native `pio test -e native` 833/833; `pio run -e cardputer -j 4` SUCCESS
(Flash 74.5%, flat vs slice-0065); flashed to /dev/ttyACM0 and both lists confirmed on device
(unified band + thumb, wrap at both ends, OS commands fire on the correct row, ducky Run launches the
selected script, no clipping at the new row height).
