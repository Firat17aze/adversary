---
id: '0065'
title: "Selection-list migration wave 1: the simple single lists"
type: slice
status: accepted
date: 2026-10-18
supersedes: []
superseded_by: []
---

## Goal

Pick up the first tranche of the deferred ledger item **"Finish the selection-list migration:
~10 screens"**. A survey (2026-10-18) found that of the ten screens item 93's sweep missed,
*none* are on the shared `ScrollableList`/`ListScrollModel` component yet: every one still
hand-rolls its own selection loop, selected-row band, and scrollbar. This wave migrates the
four lowest-risk single-list screens; the band-reconcile screens (karma, evil_twin, sniffer,
ble_bad_ble) and the hard ones (settings, ir_record) follow in later waves.

Wave 1 screens:
- **ble_apple_attack** — the Apple attack-type picker (5 fixed options).
- **saved_networks** — the saved-credentials list (two-line rows: SSID + masked password).
- **server_menu** — the 3-item server submenu.
- **badusb** — the DuckyScript picker (built-ins + SD scripts, scrollable).

The migration routes each through the shared component so they share the one selected-row band
idiom (slice-0063: component paints the band, callback draws content only), the one proportional
scrollbar thumb, and the pure `ListScrollModel` (slice-0062: one tested copy of the
selection/scroll/wrap math). It also adopts `theme::LIST_ROW_HEIGHT` (slice-0064) for the
single-line lists.

Three of these screens get a **correctness fix for free**: `saved_networks` currently paints no
scrollbar at all *and* drives its scroll window off a hardcoded "4 visible rows" that does not
match the 3 rows its 28px rows actually render (the exact drift `ListScrollModel` was extracted
to kill, see its header: commit a2417c8). `badusb` already paints a thumb but on a bespoke copy
of the geometry.

## Definition of Done

- **Given** the four wave-1 screens
- **When** each renders its list after the change
- **Then** the selected row is drawn by the shared component (`BG_SELECTED` when the list holds
  focus, `BG_SECONDARY` when the screen's footer does), the per-row callback draws content only
  and never fills its own band, and no screen hand-rolls a selection loop or scrollbar.

- **Given** the single-line lists (ble_apple_attack, server_menu, badusb)
- **When** they render
- **Then** their row height is `theme::LIST_ROW_HEIGHT` (14), replacing the bespoke 12 / 20 / 18
  each used, and every row's text is fully visible with no clipping.

- **Given** `saved_networks` (two-line rows)
- **When** it renders a list longer than the viewport
- **Then** it paints the unified scrollbar thumb (it painted none before), the visible-row count
  is whatever physically fits the rect (not a hardcoded guess), and the selection never scrolls
  off-screen.

- **Given** the selection sits on the last item and the operator presses Down (or first item +
  Up)
- **When** the input is handled
- **Then** the selection wraps to the other end on **every** wave-1 screen, matching all
  already-migrated lists (this is a deliberate behaviour change for ble_apple_attack and badusb,
  which previously clamped: see Design).

- **Given** the repository checks
- **When** the native suite and the cardputer build run
- **Then** native stays green (833/833) and the cardputer firmware compiles.

## Design

### Measured starting state (survey 2026-10-18)

| Screen | list | band idiom today | row height (name) | scrollbar today | wrap today |
|--------|------|------------------|-------------------|-----------------|------------|
| ble_apple_attack | `m_promptOptions` (5) | `BG_SELECTED`+`TEXT_INVERSE` focused / `BG_SECONDARY`+`TEXT_PRIMARY` footer-focus | `12` (bare literal) | none (fits) | clamps |
| saved_networks | saved creds | `BG_SELECTED` when `selected && !footer-focus`, else none | `28` `ENTRY_HEIGHT` (two-line) | **none** (bug) | wraps |
| server_menu | 3 fixed items | `BG_SECONDARY` band + `ACCENT` text | `20` `ITEM_HEIGHT` | none (fits) | wraps |
| badusb | script picker | `BG_SELECTED`+`TEXT_INVERSE` | `18` `ROW_H` | bespoke thumb | clamps |

### The component contract being adopted

`ScrollableList::render(canvas, model, x, y, w, h, rowHeight, drawRow, focused)` paints the
selected-row band itself (`BG_SELECTED`/`BG_SECONDARY` by `focused`), derives `visibleRows =
h / rowHeight`, feeds that viewport to the model, and draws the one proportional thumb when
`model.isScrollable()`. The screen keeps a `ListScrollModel` member, feeds it `setItemCount(...)`
each frame, drives it with `moveUp()`/`moveDown()` from `handleInput`, and reads
`selectedIndex()` wherever it needs the current row. The callback draws **content only**.

### Per-screen changes

- **ble_apple_attack**: replace the `int m_configSelection` with a `ListScrollModel m_promptList`;
  `drawConfig` calls `ScrollableList::render(..., theme::LIST_ROW_HEIGHT, cb,
  !footerHints_.hasFocus())`; `drawAttack`/`startAttack` read `m_promptList.selectedIndex()`.
  Up/Down become `moveUp`/`moveDown`.
- **saved_networks**: replace `selectedIndex_`+`scrollOffset_` with a `ListScrollModel`; the
  callback draws the two content lines (SSID at `rowTopY+4`, masked password at `rowTopY+14`),
  keeping the "selected row brightens the password colour" content cue. `ENTRY_HEIGHT` stays 28
  (two-line; **not** the single-line token, per slice-0064's two-line exclusion). The component
  supplies the missing thumb and the correct viewport.
- **server_menu**: add a `ListScrollModel`; `drawMenu` renders the preamble (SSID/key) as before,
  then the 3-item list through the component at `theme::LIST_ROW_HEIGHT`. The list never
  overflows, so `isScrollable()` is false and no thumb draws; the payoff here is purely the
  unified band. Status suffixes (`[RUNNING]`/`[ON]` etc.) keep their `SUCCESS`/`SECONDARY`
  colours in the callback.
- **badusb**: replace `m_selectedScript`+`m_scrollOffset`+`VISIBLE_ROWS` with a
  `ListScrollModel` (the component derives the visible count from the rect, so `VISIBLE_ROWS` is
  deleted, not re-homed); row height `theme::LIST_ROW_HEIGHT`; the "SD" badge keeps drawing in
  the callback.

### Deliberate behaviour and visual changes (to confirm on device)

1. **Text no longer inverts on selection.** ble_apple_attack and badusb drew selected-row text
   `TEXT_INVERSE`; the house idiom (slice-0063) is content-coloured text on the band. They become
   `TEXT_PRIMARY`/content-coloured, matching every migrated list.
2. **server_menu's band changes** from `BG_SECONDARY`+`ACCENT` text to the component band with
   content-coloured text; the band also becomes full-width (was inset 4px), matching the house
   idiom.
3. **saved_networks gains a dim band when the footer holds focus** (it drew none before) and
   **gains a scrollbar thumb**.
4. **Row heights change** on the three single-line screens (12/20/18 → 14). Downward on two,
   upward on one (ble_apple 12 → 14); all fit their rects with the viewport derived from the
   component, so nothing clips.
5. **Wrap** replaces clamp on ble_apple_attack and badusb (operator was shown this and
   recommended accepting it for consistency; confirm it feels right on device).

### Centering offset

Single-line row text is drawn at `rowTopY + 2`, matching the majority of already-migrated lists
(beacon/probe/deauth/wardriving/handshake/radio/ble_spam). The `+2`-vs-`+4` split (whitelist is
at `+4`) is a known open item — the ledger's "Shared vertical-centering offset for single-line
list rows" — and will move **all** single-line lists to one correct offset at once. This wave
deliberately does not pre-empt that: it stays a routing change, not a centering change.

### Existing code considered

Reused the shared `ScrollableList`/`ListScrollModel` (slice-0062) and `theme::LIST_ROW_HEIGHT`
(slice-0064) rather than touching any per-screen height literal in isolation. Deliberately did
**not** fold the two-line `ENTRY_HEIGHT`=28 into the single-line token (slice-0064 excluded
two-line rows for lack of a rule-of-three case). Deliberately did **not** migrate the tiny fixed
popups/action-menus elsewhere in these files (none in the wave-1 four, but the scope rule stands:
only genuine scrolling selection lists migrate; a 2-3 item menu that never scrolls buys nothing
from a scrolling component). No new abstraction introduced.

## Verification

Same shape as slice-0064: this is a display-path refactor whose behaviour is pixel layout, which
no pure-logic unit test can assert, and the repo has no rendering-verification harness. The
`ListScrollModel` selection/scroll/wrap math is already unit-tested and is **unchanged** by this
slice, so the native suite guards that the shared contract did not regress; no new unit test is
warranted (a test asserting a screen calls `render` would test the implementation, not
behaviour).

- **Native suite** — `pio test -e native` stays 833/833.
- **Cardputer build** — `pio run -e cardputer -j 4` compiles (`-j 4` is the host-RAM cap; default
  `-j 12` OOM-kills this ~5 GiB host).
- **On-device visual** — flash and confirm, per screen: the unified band on selection; wrap at
  both ends (ble_apple_attack, badusb); the new scrollbar thumb + correct viewport on
  saved_networks (populate >3 saved networks); no clipping at the new row heights; server_menu's
  3 items and status suffixes render correctly.

## As built

Shipped as designed. All four screens now keep a `ListScrollModel` and render their list through
`ScrollableList::render`, with the per-row callback drawing content only:

- **ble_apple_attack** — `m_configSelection` (int) replaced by `m_promptList`; row height
  `theme::LIST_ROW_HEIGHT` (was a bare `12`); selected-row text no longer inverts; Up/Down wrap.
- **saved_networks** — `selectedIndex_`+`scrollOffset_` replaced by `list_`; two-line rows kept at
  `ENTRY_HEIGHT`=28 (not the single-line token); gained the previously-absent scrollbar thumb; the
  viewport is now derived from the rect, replacing the hardcoded "4 visible" that did not match the
  3 rows that physically fit (the drift `ListScrollModel` exists to kill). The "selected row
  brightens the masked password" content cue was preserved.
- **server_menu** — `selection_` (int) replaced by `menuList_`; `ITEM_HEIGHT`=20 removed in favour
  of `theme::LIST_ROW_HEIGHT`; the `BG_SECONDARY`+ACCENT-text band became the house band
  (full-width); the `[RUNNING]`/`[ON]` status suffixes keep their `SUCCESS`/`SECONDARY` colours in
  the callback.
- **badusb** — `m_selectedScript`+`m_scrollOffset`+`VISIBLE_ROWS` replaced by `m_scriptList` (the
  component derives the visible count, so `VISIBLE_ROWS` and `ROW_H` were both deleted); row height
  `theme::LIST_ROW_HEIGHT` (was `18`); the bespoke scrollbar replaced by the unified thumb;
  selected-row text no longer inverts; Up/Down wrap.

Single-line row text is at `rowTopY + 2` (majority convention); the `+2`-vs-`+4` reconciliation
stays deferred to the ledger's shared-centering item, keeping this a pure routing change.

**Scope note honoured:** only genuine scrolling selection lists were migrated. No tiny fixed
popup/action-menu was routed through the component (none in these four screens; the rule stands for
later waves).

**Surfaced during on-device review (not a regression, ledgered):** badusb (and the other HID-ish
screens mouse_jiggler + server_status) never wired footer-focus input (`handleInputWithDispatch` /
`setFocus`), so their footer bar is decorative. The old badusb had the same gap; the migration only
made it visible by bringing the rest of the screen onto the house idiom. Logged as a dedicated
follow-up slice rather than folded in here (distinct concern, needs its own small design).

**Verification run:** native `pio test -e native` 833/833; `pio run -e cardputer -j 4` SUCCESS
(Flash 74.5%, flat vs slice-0064); flashed to /dev/ttyACM0 and all four screens confirmed on device
(unified band, saved_networks scrollbar + correct viewport, wrap on ble_apple_attack/badusb, no
clipping at the new row heights).
