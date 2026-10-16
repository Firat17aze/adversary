---
id: '0062'
title: "Shared scrollable-list component (unified selection, scroll window, wrap, and scrollbar)"
type: slice
status: accepted
date: 2026-10-16
supersedes: []
superseded_by: []
---

## Goal

The firmware carries 8+ hand-rolled selection lists — scanner, settings, wardriving log,
handshake history, the four attack-config screens (deauth / probe-flood / beacon-spam /
ble-spam), ble-scanner, captures, badusb, whitelist, the radio file/preset lists — and
every one re-implements the same four behaviours from scratch: a selection index, a scroll
offset, a "keep the selection on screen" windowing rule, and a right-edge scrollbar. The
duplication is not theoretical. It produced the wardriving off-screen-selection bug (the
scroll window was wider than the rows the renderer actually drew, fixed in `158246b`) and
its mirror image on the four attack-config screens (a hardcoded visible-count *below* what
the layout drew, scrolling early, fixed in `a2417c8`), and it left ~4 mutually
inconsistent scrollbar idioms visible across the app at once. The rule of three is long
past. This slice extracts the shared behaviour into one component — a pure, unit-tested
`ListScrollModel` (selection + scroll window + wrap, driven by a render-supplied viewport
height) and a thin device-side `ScrollableList` renderer that owns the single unified
scrollbar style and delegates per-row drawing back to each screen — then migrates a
**representative core set** of screens onto it to prove the seam. The remaining screens
are a tracked follow-up batch (boy-scouted as each is next touched), not this slice.

## Definition of Done

- **Given** a list of N items shown through the component with a viewport that fits V < N rows
- **When** the operator presses Down on the last visible row
- **Then** the scroll window advances by one so the newly-selected row is drawn on screen
  (the selection is never drawn outside the viewport, in either scroll direction)

- **Given** the selection is on the first item
- **When** the operator presses Up
- **Then** the selection wraps to the last item and the scroll window jumps so that last
  item is on screen (and symmetrically, Down on the last item wraps to the first)

- **Given** a list whose item count is ≤ the viewport (every item fits)
- **When** the operator navigates with Up/Down
- **Then** the scroll offset stays at zero and no scrollbar track is drawn — the list never
  scrolls when nothing is off screen

- **Given** any two of the migrated core screens scrolling a list longer than their viewport
- **When** each is viewed on device
- **Then** both draw the identical scrollbar idiom (same width, same proportional thumb with
  the same minimum thumb height), replacing the previously divergent per-screen idioms

## Design

### What exists and was considered

Searched `src/ui/components/` and `src/ui/screens/`. There is **no** generic list component
today: `components/` holds `action_menu`, `carousel_menu`, `character_selector`,
`footer_hints`, `menu`, `progress_bar`, `pulse_strip`, `status_bar`, `text_input_popup`,
`toast_manager`. `ActionMenu` and `Menu` each own a `selectedIndex_`, but they are concrete
menus (fixed command lists, their own render), not a reusable viewport-scrolling list, and
neither is used by the data-list screens above. So this is net-new infrastructure, justified
by measured duplication (8+ sites) and two shipped bugs — not speculative generality.

The **single-source-of-truth** rule the recent fixes converged on is the design's core: the
renderer is the only thing that knows how many rows physically fit (it owns the layout rect
and row height), so the visible-count must be *derived there each frame* and the
scroll/nav logic must *read that derived value* — never a second hardcoded copy that can
drift. The component makes that structural instead of a per-screen convention.

Explicitly **out of scope**: pure text scrollers with no selection — the `about` screen's
line scroll, the RFID tag-detail view, and the deferred BadUSB on-device script viewer
(`LEDGER.md`, "needs a scrollable text-viewer component"). Those scroll a text buffer, not a
selected item, and forcing them through a selection model would be the wrong abstraction.

### Shape

Two files, mirroring the display-free-core precedent already in the native build
(`ui/components/menu.cpp`, `ui/components/pulse_strip.cpp` — pure logic compiled natively,
`render()` guarded off-device, see `platformio.ini` `[env:native]`):

- **`ui/components/list_scroll_model.{h,cpp}` — pure, native-tested.** Holds `itemCount`,
  `selectedIndex`, `scrollOffset`, and a wrap flag. Caller feeds it the viewport height in
  rows each frame (`setViewport(visibleRows)`); the model clamps the offset so the selection
  is always inside `[scrollOffset, scrollOffset + visibleRows)`. API: `setItemCount(n)`,
  `setViewport(rows)`, `moveUp()`, `moveDown()`, `selectedIndex()`, `scrollOffset()`, and
  the scrollbar geometry helpers `thumbHeightPx(trackPx)` / `thumbTopPx(trackPx)` (pure math,
  so the thumb is unit-tested too). No Canvas, no Arduino — compiles and tests on the host.

- **`ui/components/scrollable_list.{h,cpp}` — device-side renderer.** Owns a
  `ListScrollModel`. Given a `Canvas`, a content rect, and a row height, it (1) derives
  `visibleRows = rectHeight / rowHeight` and pushes it into the model — the single source of
  truth — (2) draws the one unified scrollbar when `itemCount > visibleRows`, and (3) calls a
  per-row draw callback `void(Canvas&, int itemIndex, int16_t rowY, bool selected)` for each
  visible row. Row *appearance* stays with each screen; only the mechanics move. `render()`
  is device-guarded like the other native-compiled components.

### Unified scrollbar

One idiom replaces the four surveyed (proportional-thumb 4px/min-6 in
captures/settings/badusb/ble-bad-ble; thin 3px/no-min bar in wardriving/handshake; up/down
triangles in scanner/rfid/deauth/probe-flood/beacon-spam/ble-spam; "N/total" text in about):
**a proportional thumb, 4 px wide, minimum thumb height 6 px, on a right-edge track.** Chosen
because it is already the plurality idiom and it encodes both *where* you are and *how much*
is off screen — the triangles encode only "more exists" and the text idiom is cramped. The
two dimensions are named constants on `ScrollableList`, not magic numbers.

### Core migration set (this slice)

Three **uniform** selection lists, picked to exercise the variety the component must survive,
not for being the easiest:

- **wardriving log** — the full-screen data list that carried the off-screen bug; thin-bar
  idiom; external ring-buffer data; selection drawn by the row callback.
- **scanner** — the triangle idiom, the most different from the target thumb, so it proves the
  renderer handles a screen whose scrollbar *changes* appearance on migration; also the
  hardest integration (a public `getSelectedIndex()`, a live external result count that
  changes under the list, and number-key jump) — if the seam survives scanner it survives.
- **deauth config** — a config list living inside a non-list screen (CONFIG state, variable
  list origin below a target block), so it proves the component composes into a screen that is
  not purely a list; migrating it structurally closes the premature-scroll bug family the
  attack-config screens shared (commit a2417c8).

**Settings was considered for the core set and deliberately left out.** Its list mixes
selectable rows with non-selectable HEADER/SEPARATOR rows and navigates with a
`nextSelectable(idx, dir)` that skips them — the same shape as `Menu`. The `ListScrollModel`
intentionally treats every item as selectable; giving it a skip-predicate to fit settings
would be generalising the model for a single screen (only settings and the already-excluded
`Menu` need it — two, not the rule-of-three threshold). Settings therefore migrates later, if
and when a third skip-list appears to justify a selectable-predicate variant. The
proportional-thumb "visual no-op" proof it would have given moves to the follow-up batch, whose
**first** item is captures (a uniform proportional-thumb list), so the no-op is confirmed there.

The remaining selection lists (captures, badusb, ble-scanner, ble-spam, probe-flood,
beacon-spam, handshake history, whitelist, radio lists) are that follow-up batch — see
`## As built` / `LEDGER.md` at merge — each migrated when next touched, so no screen is
churned twice and each conversion is individually verifiable on device.

## Verification

**Pure model — native unit suite** (`test/test_list_scroll_model/`, added to the native
`build_src_filter` beside the other display-free components). Each test encodes a rule a
hand-rolled list previously got wrong, so it fails against the absent/naive model and passes
against the component:

- `test_down_on_last_visible_row_advances_window` — selection stays inside the viewport
  scrolling down (the wardriving off-screen bug, `158246b`).
- `test_up_scrolls_window_back_into_view` — symmetric upward windowing.
- `test_count_fits_viewport_never_scrolls` — `itemCount ≤ visibleRows` ⇒ offset stays 0
  (the premature-scroll bug, `a2417c8`).
- `test_up_on_first_wraps_to_last_and_shows_it` / `test_down_on_last_wraps_to_first` — wrap
  policy plus the window jump that keeps the wrapped selection on screen.
- `test_shrinking_viewport_reclamps_offset` — viewport smaller than last frame re-clamps so
  the selection is still visible (layout-driven visible-count, the single-source rule).
- `test_thumb_height_is_proportional_with_minimum` / `test_thumb_top_tracks_offset` — the
  scrollbar geometry math, including the min-height clamp.

**Renderer + migrations — on device.** The `ScrollableList` renderer and the migrated
screens touch `Canvas`, so they are outside the native build (per the established UI-screen
convention, ADR-recorded) and are verified on hardware against the Definition-of-Done
scenarios: on the three migrated screens, scroll a list longer than its viewport and confirm
(a) the selection never leaves the screen in either direction, (b) Up/Down wrap at the ends,
(c) a list that fits shows no scrollbar and never scrolls, and (d) all three draw the
identical thumb. Build must be green for both `native` (`pio test -e native`) and `cardputer`
before merge.

## As built

Shipped the two-file component as designed. `ui/components/list_scroll_model.{h,cpp}` is the
pure selection/scroll/wrap state with the thumb geometry (MIN_THUMB_PX = 6); it is compiled
into the native build and covered by `test/test_list_scroll_model/` — **8 unit tests, all
green** — each encoding a rule a hand-rolled list had got wrong (window follows selection down
and up, a list that fits never scrolls, a shrunk viewport re-clamps, wrap at both ends with the
window jump, and the proportional-thumb-with-minimum math). `ui/components/scrollable_list.h`
is the ESP32-guarded renderer: it derives the viewport from the layout rect (the single source
of truth), calls a per-row draw callback, and draws the one unified scrollbar — a 4 px
proportional thumb, replacing the four divergent idioms.

Migrated three screens: **wardriving log**, **scanner**, and **deauth config**. Each now owns a
`ListScrollModel`, renders through `ScrollableList`, and wraps at the ends (none did before).
All of their hand-rolled scroll windows, visible-count copies, and bespoke scrollbars
(wardriving's 3 px bar, scanner's up/down triangles, deauth's `^`/`v` markers) are gone.

Definition of Done, confirmed on device (Cardputer ADV, 2026-10-16):
- Down past the last visible row keeps the selection on screen (verified on the wardriving log,
  the 7-row list that carried the original off-screen bug) — **met**.
- Up on the first row wraps to the last and scrolls it into view (and the reverse) — **met** on
  all three screens.
- A list that fits shows no scrollbar and never scrolls — **met** (deauth's 5 options fit; no
  thumb drawn).
- All three migrated screens draw the identical thumb — **met**.

Deviation from the planned core set: **settings was dropped.** It is a header-skip list
(non-selectable HEADER/SEPARATOR rows navigated via `nextSelectable`), so fitting it would have
meant adding a selectable-predicate to the model for a single screen — speculative generality
(only settings and the excluded `Menu` need it). It is deferred until a third skip-list
justifies that variant. The proportional-thumb "visual no-op" proof it would have given moves to
the follow-up batch (captures).

Two defects surfaced during on-device verification and were fixed in the same work:
- The wardriving selected row drew a bright `ACCENT()` bar — the lone outlier against the
  app-wide `BG_SELECTED()` standard (scanner, deauth, ble/radio/…). Switched it to
  `BG_SELECTED()` so the three migrated lists match.
- The log's right-side `n/n` footer count was sticky: `FooterHints::setRightContent` was set only
  in the log view and never cleared, so the count bled into the scanning/paused/idle screens.
  Fixed by clearing the right content once per frame at the top of `render()` (only the log sets
  it), and the count now reads the highlighted item's position (`selectedIndex + 1`) rather than
  the scroll offset.

The remaining selection lists (captures, rfid, ble-scanner, ble-spam, probe-flood, beacon-spam,
handshake history, whitelist, radio lists) remain hand-rolled and are tracked in `LEDGER.md` as
the follow-up batch; a separate ledger item tracks absorbing the selected-row highlight into the
component once the config-trio batch makes its footer-focus dim a rule-of-three case, and another
tracks a modal/dialog selected-item consistency audit (those are not `ScrollableList` lists).
