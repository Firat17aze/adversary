---
id: '0077'
title: "Route the sniffer action menu through the shared ActionMenu and modernise its selected row"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

Close the ledger feature "Route the sniffer action-menu overlay through the shared `ActionMenu`
component" (slice-0068 wave-2c deferral). The sniffer's packet-log quick-action popup
(`SnifferScreenState::ACTION_MENU`, built by `buildActionList` from the selected packet's frame
type) is the lone popup action menu in the app that still hand-rolls its own selection band, nav,
and draw: seven other screens (scanner, ble_scanner, saved_networks, wardriving, settings,
captures, server_menu) already route through the shared `ui::ActionMenu`. This migration removes
the eighth, divergent copy.

The ledger's open question was whether the component needs to learn the frame-type-conditional
action set. It does not: the conditionality lives in the *caller* (`buildActionList` picks which
actions to add), exactly as `scanner_screen` conditionally adds Connect/Disconnect and
`ble_scanner` conditionally adds Apple Attack. The shared component already supports per-context
item sets, so no new component capability is required.

The slice also flips the shared `ActionMenu`'s selected-row band from the pre-slice-0063
`ACCENT()`-fill-with-inverted-text idiom to the house `BG_SELECTED()` band with content-coloured
text. This is the component half of the ledger bug "Selected-item style inconsistency in
modals/dialogs": doing it here keeps the sniffer from regressing to the old band (its hand-rolled
overlay already used `BG_SELECTED()`), and modernises all eight `ActionMenu` popups in one move
(operator decision 2026-10-19: route + modernise in one slice).

## Definition of Done

- **Given** a captured beacon or probe-response packet selected in the packet log
- **When** the operator presses ENTER
- **Then** the shared `ui::ActionMenu` opens listing the frame-type-appropriate actions
  (Handshake Capture, Deauth, Evil Twin), rendered by the shared component, with no
  `SnifferScreenState::ACTION_MENU` state and no bespoke `drawActionMenu`/`m_availableActions`
  remaining

- **Given** the sniffer action menu is open
- **When** the operator presses UP at the top row or DOWN at the bottom row, or presses an
  action's shortcut key (e.g. `D` for Deauth)
- **Then** the selection wraps and skips disabled rows (matching the other `ActionMenu` popups),
  and the shortcut fires its action directly

- **Given** the sniffer action menu is open
- **When** the operator selects an action with ENTER or a shortcut, or dismisses with ESC/`` ` ``
- **Then** selecting launches the same attack on the snapshotted packet as before (the
  native-tested `makeSnifferAttackTarget` policy is unchanged), and dismissing returns to the
  packet log

- **Given** any `ActionMenu` popup (the sniffer's or the seven existing consumers')
- **When** a row is selected
- **Then** it renders with the slice-0063 `BG_SELECTED()` band and content-coloured text, not the
  pre-0063 `ACCENT()`-fill inverted-text band

## Design

**Route the overlay through `ui::ActionMenu`, gated like scanner.** The sniffer gains a
`ui::ActionMenu actionMenu_` member and follows `scanner_screen`'s exact shape: the overlay is
gated on `actionMenu_.isVisible()` (not a sibling screen state), intercepts input first in
`handleInput`, and renders on top after the base view. This retires the bespoke
`SnifferScreenState::ACTION_MENU` enumerator, the `m_availableActions[6]` / `m_actionCount` /
`m_actionMenuSelection` fields, the `drawActionMenu` method, and the four `ACTION_MENU` input
branches (ENTER/ESC/`;`/`.`). The base state while the menu is open stays `PACKET_LOG`, which is
the correct model: the menu is an overlay, not a sibling state.

**Frame-type conditionality stays in the caller.** `buildActionList(packet)` is rewritten to
`actionMenu_.clearItems()` then `addItem(shortcut, label, true)` for each action its existing
frame-type `switch` selects, then `actionMenu_.show()`. No `ActionMenu` change is needed for the
conditional set: the caller already owns that choice, matching the other consumers. ENTER on the
packet log snapshots the selected packet (unchanged, slice-0070 physical-index resolve) and calls
this builder.

**Keep the native-tested policy untouched.** `makeSnifferAttackTarget` (native-tested in
`test_sniffer_actions`, maps `PacketAction` -> `ScreenId`, returns false for CANCEL) and
`executeAction` are unchanged. Only the render + input layer moves to the component. The
`actionMenu_.setOnAction(char shortcut)` callback translates the shortcut back to a `PacketAction`
through one shared source of truth and calls the unchanged `executeAction`; `setOnDismiss` returns
to the log with a redraw.

**One source of truth for the action triple.** The `PacketAction` <-> shortcut <-> label mapping
is expressed once (a small fixed table or paired helpers) and read in both directions: forward to
build the menu rows, reverse in the callback. Shortcuts follow the established scanner convention
(`H` Handshake Capture, `D` Deauth, `T` Evil Twin) plus `K` for Karma. A shortcut the component
reports that is not in the table is a programming error (the component only calls back with a
shortcut it rendered), so the reverse path fails loud rather than silently launching a wrong or
no-op attack.

**No dead Cancel-only menu.** The old `buildActionList` always appended a `CANCEL` row, so a
frame type with no attack (DEAUTH, data) still opened a one-row Cancel-only menu. With the shared
component, ESC/`` ` `` already dismisses (the component draws an `ESC:Back` footer), so the
explicit `CANCEL` row is dropped (matching scanner), and ENTER simply does not open the menu when
the frame-type action set is empty. `makeSnifferAttackTarget(CANCEL)` stays covered by its native
test; only the UI stops emitting a Cancel row.

**Modernise the shared band (component half of the modal-style ledger item).** `ActionMenu::render`
currently fills the selected row with `theme::ACCENT()` and inverts the text to
`theme::BG_PRIMARY()`. It is changed to the slice-0063 standard: a `theme::BG_SELECTED()` band with
the text left at its content colour (`theme::TEXT_PRIMARY()`), the same band the sniffer's
hand-rolled overlay already drew and that every data list uses since slice-0063. Because all eight
popups render through this one method, the change unifies them at once and prevents the sniffer
from regressing when it adopts the component.

**What existing code was considered.** `ui::ActionMenu` (`action_menu.h/.cpp`, 7 consumers) is the
established popup idiom and is reused wholesale; its `navigateUp`/`navigateDown` already wrap and
skip disabled rows and its `handleInput` already supports shortcut quick-keys, so the sniffer gains
behaviour rather than losing it. `ScrollableList` was considered and rejected: the action set is a
fixed, non-scrolling <=4-row popup, not a scroll window, and `ActionMenu` (not `ScrollableList`) is
the component the other seven popups use, so routing here onto `ScrollableList` would create a new
inconsistency rather than remove one. Keeping the hand-rolled overlay was rejected: it is the lone
eighth copy, with poorer navigation (no wrap, no skip-disabled, no shortcuts) and a now-divergent
state model.

## Verification

- `pio run -e cardputer` SUCCESS and fits: `sniffer_screen.cpp` and `action_menu.h` are under
  `ui/`, which the native `build_src_filter` excludes (`-<ui/>`), so this carries no host test, as
  recorded for the slice-0069/0072/0073/0074/0075/0076 UI work.
- `pio test -e native` stays green: `makeSnifferAttackTarget` (the only native-compiled sniffer
  policy, `test_sniffer_actions`) is unchanged, and the migration touches only the `ui/` render +
  input layer plus the `ActionMenu` band colour, none of it host-compiled logic. Exact count
  recorded in As built.
- `node scripts/lint-docs.mjs` 0 errors.
- On-device DoD (the overlay behaviour and the band colour the host cannot assert, verified by eye
  on the Cardputer): ENTER over a beacon/probe-resp opens the shared popup with the frame-type
  actions; `;`/`.` wrap and skip disabled rows; a shortcut key fires its action; ENTER/shortcut
  launches the same attack as before on the snapshotted packet; ESC dismisses; the selected row
  shows the `BG_SELECTED()` band (not the old ACCENT-inverted band) in the sniffer and in at least
  one other `ActionMenu` popup (e.g. scanner) to confirm the shared restyle.

## As built

Shipped as designed. The sniffer's packet-log quick-action overlay now routes through the shared
`ui::ActionMenu`: the `SnifferScreenState::ACTION_MENU` enumerator, the `m_availableActions[6]` /
`m_actionCount` / `m_actionMenuSelection` fields, the bespoke `drawActionMenu`, and the four
`ACTION_MENU` input branches are gone. The overlay is gated on `actionMenu_.isVisible()`, intercepts
input first in `handleInput`, and renders on top after the base view, exactly like `scanner_screen`
(`sniffer_screen.cpp` constructor wiring, `handleInput` intercept, `render` tail).

- **DoD 1 (ENTER opens the shared menu, no bespoke state):** met. ENTER on a selected beacon/probe-resp
  snapshots the packet and calls `buildActionList`, which `clearItems()` + `addItem()`s the frame-type
  action set and `show()`s the shared component. No `SnifferScreenState::ACTION_MENU` or
  `drawActionMenu` remains (grep-clean).
- **DoD 2 (wrap + skip-disabled + shortcut keys):** met by reuse. The shared component's
  `navigateUp`/`navigateDown` already wrap and skip disabled rows, and `handleInput` already fires a
  row by its shortcut key; the sniffer gained these for free. Shortcuts follow the scanner convention
  (`H`/`D`/`T`, plus `K` for Karma), expressed once in the `SNIFFER_ACTION_KEYS` table read in both
  directions; `actionFromShortcut` fails loud (`LOG_E`) on an unmapped key.
- **DoD 3 (select launches the same attack, ESC dismisses):** met. The `setOnAction` callback maps the
  shortcut back to a `PacketAction` and runs the unchanged, native-tested `makeSnifferAttackTarget`
  policy via `executeAction`; `setOnDismiss` repaints the log. A frame type with no attack no longer
  opens a dead Cancel-only menu.
- **DoD 4 (BG_SELECTED band on all popups):** met. `ActionMenu::render`'s selected row now fills
  `theme::BG_SELECTED()` with `theme::TEXT_PRIMARY()` text (was `theme::ACCENT()` fill with inverted
  `BG_PRIMARY` text). Because all eight popups share this one method, the restyle is uniform; the
  sniffer popup was confirmed on-device, which exercises the shared path.

Verification: `pio run -e cardputer` SUCCESS, Flash 74.6% (2,493,771 B of 3,342,336); `pio test -e
native` 844/844 (`makeSnifferAttackTarget` unchanged, `test_sniffer_actions` green); `node
scripts/lint-docs.mjs` 0 errors. On-device (operator, 2026-10-19): the overlay opens over a
beacon/probe-resp with the frame-type actions, navigation/shortcut/select launch the attack, ESC
dismisses, and the band renders in the new style ("looks perfect").

On-device testing of this flow also surfaced a pre-existing, unrelated heap-corruption data race in
the sniffer *capture* path (WiFi-task promiscuous callback mutating main-loop-shared containers with
no lock). It is independent of this slice (the action-menu reroute never touches the capture
plumbing) and is logged to the ledger for its own root-cause slice.
