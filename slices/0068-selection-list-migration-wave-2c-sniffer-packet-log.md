---
id: '0068'
title: "Selection-list migration wave 2c: the sniffer packet log"
type: slice
status: accepted
date: 2026-10-18
supersedes: []
superseded_by: []
---

## Goal

Continue the deferred ledger item **"Finish the selection-list migration"** (item 94). Waves 1
(slice-0065), 2a (slice-0066) and 2b (slice-0067) routed the simple lists, the two BadBLE lists
and the karma clients list. This wave migrates the **sniffer packet log**
(`SnifferScreenState::PACKET_LOG` in `sniffer_screen.cpp`), the HIGH-risk entry the 2026-10-18
survey flagged: its selection drives a dependent action menu, and its backing store is a
fixed-capacity ring that mutates in place as frames arrive.

The selection-to-action correctness hazard (the action menu resolving its target packet by a live
ring index that can drift to a different packet) was fixed first, on its own, in commit `ae8bd11`
(`fix(sniffer): act on a snapshot of the selected packet, not a live ring index`). This slice is
the pure display-path migration that builds on that fix: it routes the list rendering, scrolling
and navigation through the shared `ScrollableList`/`ListScrollModel` component.

Two findings from the survey are deliberately **not** in this wave and are tracked in the ledger:
the action menu overlay is itself a small (<=6-row, non-scrolling) selection list, deferred (it is
a weak fit for a scrolling-list component and may instead route through the shared `ActionMenu`);
and once the ring wraps, rows display in physical array order rather than chronological order, a
separate usability bug orthogonal to the list component.

This wave does not close item 94: the settings and ir_record screens (wave 3) remain.

## Definition of Done

- **Given** the sniffer packet log (`PACKET_LOG` state) with captured packets
- **When** it renders
- **Then** the selected row's band is drawn by the shared component (`BG_SELECTED` when the list
  holds focus, `BG_SECONDARY` when the footer or the action menu does), the per-row callback draws
  content only (timestamp, frame-type code, SSID, flags, source-MAC tail, RSSI) and never fills
  its own band, and the screen no longer hand-rolls a visible-row loop or a scrollbar.

- **Given** the deauth-flood and/or [PAUSED] banners are showing above the list
- **When** the packet log renders
- **Then** the visible-row count is whatever physically fits the list rect *below the banners* at
  `theme::LIST_ROW_HEIGHT` (the rect height shrinks as banners appear), not the hardcoded `5` that
  ignored them, and the bottom row never collides with the footer.

- **Given** the selection sits on the last visible packet and the operator presses Down (or the
  first packet + Up)
- **When** the input is handled
- **Then** the selection wraps to the other end (a deliberate behaviour change: the log clamped
  before), matching every already-migrated list.

- **Given** capture is running and unpaused with the selection at the tail
- **When** a new packet arrives
- **Then** the selection continues to auto-follow the newest row (behaviour preserved), and the
  scroll window keeps the selection on screen.

- **Given** a packet is selected and the operator opens the action menu
- **When** the menu overlay is shown
- **Then** the action still acts on the packet snapshotted at Enter (the `ae8bd11` fix is intact),
  and the list underneath renders unfocused (its band dims to `BG_SECONDARY`).

- **Given** the repository checks
- **When** the native suite and the cardputer build run
- **Then** native stays green (840/840) and the cardputer firmware compiles.

## Design

### Measured starting state (sniffer_screen.cpp, survey 2026-10-18 + re-read post-ae8bd11)

| Aspect | Today | File:line |
|--------|-------|-----------|
| list source | `std::vector<PacketSummary> m_packetLog` used as a fixed 25-entry in-place ring | `.h` ~238 |
| selection | `size_t m_packetLogSelection` | `.h` ~240 |
| scroll offset | `size_t m_packetLogScroll` | `.h` ~241 |
| band idiom | callback self-paints `BG_SELECTED` fill; SSID flips `TEXT_PRIMARY`/`TEXT_SECONDARY` on selection; other columns keep semantic colours | `drawPacketLogEntry` 1100, 1119 |
| row height | `LOG_ROW_HEIGHT = 18` (named constant) | `.h` ~254 |
| visible rows | `LOG_VISIBLE_ROWS = 5` (hardcoded, ignores the banners above the list) | `.h` ~253 |
| scrollbar | hand-rolled outline rect + proportional thumb (`drawLogScrollbar`) | 1153-1172 |
| wrap | **clamps** (up: `> 0`; down: `< size - 1`) | 273-292 |
| list top | **dynamic**: `HEADER_HEIGHT + 2`, pushed down by the deauth-flood (+16) and [PAUSED] (+12) banners | `drawPacketLog` 1045-1060 |
| auto-follow | unpaused + selection `>= size - 2` snaps selection/scroll to the tail | `addPacketToLog` 758-761 |
| footer focus | wired unconditionally, all states | `handleInput` ~214-221 |
| downstream use | at Enter: snapshot `m_selectedPacket` then build the action menu (post-`ae8bd11`) | 222 |

### The dynamic list rect (what makes this different from waves 1/2b)

Unlike the karma clients list (fixed top at y=22), the packet log's top is not constant: the
deauth-flood alert banner and the [PAUSED] banner render *above* the list and push it down. The
hardcoded `LOG_VISIBLE_ROWS = 5` ignored this, so with a banner showing, five 18px rows starting
lower could run under the footer. Deriving the viewport from the live rect fixes this for free:
compute `listTop` after the banner logic, pass `listH = canvas.height() - ui::FOOTER_HEIGHT -
listTop` to `ScrollableList::render`, and the component sets `visibleRows = listH / rowHeight`,
shrinking the window exactly when a banner steals vertical space.

### Per-list changes

- Replace `size_t m_packetLogSelection` + `size_t m_packetLogScroll` with a single
  `ListScrollModel m_packetLogList` (`m_` prefix to match this file's convention, not the
  trailing-underscore BadBLE/karma use). `m_packetLogHead` (the ring write cursor) and
  `m_selectedPacket` (the snapshot) are unchanged.
- `drawPacketLog`: keep the banner logic and the empty-state early return (`No packets yet`);
  replace the hand-rolled visible-row loop + `drawLogScrollbar` call with
  `ScrollableList::render(canvas, m_packetLogList, 0, listTop, canvas.width(), listH,
  theme::LIST_ROW_HEIGHT, cb, focused)`, feeding `setItemCount(m_packetLog.size())` each frame.
  `focused = (m_screenState == PACKET_LOG) && !footerHints_.hasFocus()` so the list dims when the
  action menu overlay is up. Keep the `[sel/total]` counter, reading
  `m_packetLogList.selectedIndex()`.
- `drawPacketLogEntry`: drop the `selected` parameter and the self-painted band (`fillRect(...,
  BG_SELECTED)`); the SSID column becomes a constant `TEXT_PRIMARY` (the band, not the text,
  signals selection). All other columns keep their semantic colours (frame-type colour, RSSI
  signal colour, MAC `TEXT_DISABLED`, timestamp `TEXT_SECONDARY`, `[W]` `ACCENT`).
- `drawLogScrollbar`: deleted (method + declaration + call); the shared component draws the thumb.
- Nav `;` (up) / `.` (down): the `PACKET_LOG` branch becomes
  `if (!m_packetLog.empty()) { m_packetLogList.setItemCount(m_packetLog.size());
  m_packetLogList.moveUp()/moveDown(); }` (wrap). The `ACTION_MENU` branch (the deferred action
  menu) is untouched.
- `addPacketToLog` auto-follow: feed the model instead of the raw members:
  `m_packetLogList.setItemCount(n); if (following) m_packetLogList.setSelectedIndex(n - 1)`,
  where `following = !m_logPaused && selectedIndex() >= n - 2`. Semantics preserved (including the
  pre-existing "tail = last physical index" behaviour, which the deferred ordering bug owns).
- Clear (`c`): `m_packetLogList.setItemCount(0); setSelectedIndex(0)` replaces the two raw zeroes;
  `m_packetLogHead = 0` stays.
- Enter snapshot: `m_packetLog[m_packetLogList.selectedIndex()]` replaces
  `m_packetLog[m_packetLogSelection]`.
- ctor: drop the `m_packetLogSelection(0)` / `m_packetLogScroll(0)` initialisers (the model
  default-constructs); remove the now-unused `LOG_VISIBLE_ROWS` and `LOG_ROW_HEIGHT` constants.

### Deliberate behaviour and visual changes (to confirm on device)

1. **Row height 18 -> 14** (`theme::LIST_ROW_HEIGHT`); more packets fit, and the visible count is
   derived from the rect (and shrinks under the banners) instead of the hardcoded `5`.
2. **Band reconciled** to the house idiom (component-painted, `BG_SELECTED`/`BG_SECONDARY`); the
   SSID no longer brightens on selection; semantic column colours are unchanged.
3. **List dims under the action menu** (`focused = false` in `ACTION_MENU`), signalling that focus
   moved to the overlay (it stayed fully highlighted before).
4. **Scrollbar unified** to the single proportional thumb (replacing the bespoke outline bar).
5. **Wrap** replaces clamp at both ends.

### Existing code considered

Reused the shared `ScrollableList`/`ListScrollModel` (slice-0062) and `theme::LIST_ROW_HEIGHT`
(slice-0064), as in waves 1/2a/2b. Deliberately did **not** migrate the action menu overlay: it is
a fixed <=6-row, non-scrolling selection list, a different shape from a scrolling log, and is
tracked by the ledger (it may route through the shared `ActionMenu` component instead). Deliberately
did **not** fix the ring's physical-vs-chronological display order (tracked separately) or touch
`STATS`. The `ae8bd11` snapshot fix is relied on, not re-done. No new abstraction introduced.

## Verification

Same shape as slices 0064-0067: a display-path refactor whose behaviour is pixel layout, which no
pure-logic unit test can assert, and the repo has no rendering-verification harness (the native
build excludes screens). The `ListScrollModel` selection/scroll/wrap math is already unit-tested
and is unchanged by this slice, so the native suite guards that the shared contract did not
regress; no new unit test is warranted here (the quick-action policy extracted for `ae8bd11` is
covered by `test_sniffer_actions`).

- **Native suite** - `pio test -e native` stays 840/840.
- **Cardputer build** - `pio run -e cardputer -j 4` compiles (`-j 4` is the host-RAM cap; default
  `-j 12` OOM-kills this ~5 GiB host).
- **On-device visual** - flash and, with packets captured: the unified band on selection; the new
  thumb + correct viewport (including the shrink when the deauth-flood/[PAUSED] banners show); wrap
  at both ends; auto-follow at the tail while unpaused; opening the action menu dims the list
  underneath; and launching an attack still targets the packet that was selected.

## As built

Shipped as designed. The sniffer packet log now renders through
`ScrollableList`/`ListScrollModel`: `size_t m_packetLogSelection` + `size_t m_packetLogScroll`
collapsed into one `ListScrollModel m_packetLogList`, and the hand-rolled visible-row loop plus
`drawLogScrollbar` are gone (method, declaration and call deleted).

Each Definition-of-Done scenario was confirmed on device (2026-10-18):

- **Band** - the selected row's band is painted by the component (`BG_SELECTED` focused,
  `BG_SECONDARY` under the action menu); `drawPacketLogEntry` lost its `selected` parameter and its
  self-painted `fillRect`, and the SSID column is now a constant `theme::TEXT_PRIMARY()`. Frame-type
  and RSSI colours stayed (they encode meaning, not selection).
- **Dynamic rect under the banners** - `listH = canvas.height() - ui::FOOTER_HEIGHT - listTop` is
  computed after the deauth-flood/[PAUSED] banner logic, so the component derives
  `visibleRows = listH / theme::LIST_ROW_HEIGHT` and the window shrinks exactly when a banner
  appears; no row collides with the footer (the old hardcoded `5` could).
- **Wrap** - Down past the last row wraps to the first and Up past the first wraps to the last
  (was a clamp).
- **Auto-follow** - unpaused with the selection at the tail, new packets keep the newest row
  selected and visible (`following = !m_logPaused && selectedIndex() >= n - 2`).
- **Action menu intact** - the menu still acts on the `m_selectedPacket` snapshot taken at Enter
  (the `ae8bd11` fix), and the list underneath dims to `BG_SECONDARY`
  (`focused = (state == PACKET_LOG) && !footerHints_.hasFocus()`).
- **Checks** - native suite 840/840; cardputer firmware compiles (`-j 4`), Flash 74.6%
  (2,491,751 bytes, 72 bytes smaller than the `ae8bd11` fix build).

Incidental fixes the migration delivered: the empty-log Down-key underflow is now guarded by
`!m_packetLog.empty()`, and the banner-overlap bug is resolved by deriving the viewport from the
shrinking rect. Row height moved 18 -> 14 (`theme::LIST_ROW_HEIGHT`), so more packets fit.

Deferred as planned (both logged to the ledger, not done here): the action-menu overlay (a fixed
<=6-row non-scrolling list, a weak fit for a scrolling component; may route through the shared
`ActionMenu`), and the ring's physical-vs-chronological display order once it wraps. Item 94 stays
open: the settings and ir_record screens (wave 3) remain.
