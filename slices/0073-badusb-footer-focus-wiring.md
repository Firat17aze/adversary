---
id: '0073'
title: "BadUSB script list gains footer focus; the two listless HID screens stay a static hint bar"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

Three HID-adjacent screens (`badusb`, `mouse_jiggler`, `server_status`) render a footer hint bar
but never wire footer focus: they call `m_footer.setHints()`/`render()` yet never
`handleInputWithDispatch`/`setFocus`, so SPACE cannot move focus to the footer and (where the
screen has a list) the selected-row band never dims, unlike every other list screen since the
slice-0063/0065 migration. This closes the open ledger follow-up for all three, but it does so
with a per-screen decision rather than uniform wiring, because only one of them is a list screen.

`badusb` in its IDLE state is a genuine `ScrollableList` (the script picker) whose band should
yield focus like every sibling list, and where SPACE is currently a dead key while it toggles
footer focus everywhere else: that is a real consistency gap, so it gets wired. `mouse_jiggler`
and `server_status` have no list and a footer of only universal direct-key actions (Activate =
Enter, Stop/Back = Esc): a focusable footer there would let the operator SPACE-highlight a lone
badge and Enter it, adding a focus state that does nothing a direct key does not already do. Per
KISS (complexity must earn its place), those two stay a static hint bar by decision, recorded
here so the ledger line can close.

## Definition of Done

- **Given** the BadUSB screen in its IDLE script-selection state with the footer unfocused
- **When** the operator presses SPACE
- **Then** input focus moves to the footer, the script-list selected-row band dims to
  `BG_SECONDARY` (matching every migrated sibling list under footer focus), and `;`/`.` cycle the
  footer hints instead of moving the list selection

- **Given** the BadUSB screen in its pre-activation CONFIRM state (USB HID not yet activated), a
  two-action decision screen (Activate / Back) with no list
- **When** the operator presses SPACE
- **Then** focus moves to the footer and the focused hint badge highlights, so SPACE behaves the
  same in every interactive state of the screen, not only the list state (the gap found on device
  in the first cut, which wired the dispatch in IDLE alone)

- **Given** the BadUSB IDLE footer holds focus on a hint
- **When** the operator presses Enter
- **Then** that hint's action fires (Run runs the selected script, Back exits) and focus returns
  to the list

- **Given** the BadUSB IDLE state with the footer unfocused
- **When** the operator uses `;`/`.` and Enter directly
- **Then** the list navigates and the selected script runs exactly as before (the direct-key path
  is unchanged; footer focus is purely additive)

- **Given** the BadUSB screen is re-entered after being left with the footer focused
- **When** it shows
- **Then** the footer starts unfocused (the band is the full `BG_SELECTED` highlight), so focus
  state never leaks across visits

- **Given** the `mouse_jiggler` or `server_status` screen
- **When** the operator presses SPACE
- **Then** nothing changes (these screens have no list and a single universal footer action, so
  they remain a static hint bar by the decision recorded in Design)

## Design

The footer-focus idiom (scanner/sniffer/handshake and the rest) is: `handleInput` opens with a
dispatch block, `m_footer.handleInputWithDispatch(key, action)`, so SPACE toggles focus, `;`/`.`
cycle hints, Enter dispatches the selected action; and the list render receives
`!m_footer.hasFocus()` as its `focused` argument so `ScrollableList` dims the band while the
footer holds focus. `badusb` already owns a `ui::FooterHints m_footer` and a `ScrollableList`
script picker, so the wiring is confined to `badusb_screen.cpp` with no header change:

1. `show()` seeds `m_footer.setFocus(false)` so focus never carries over between visits.
2. `drawIdle()` passes `!m_footer.hasFocus()` as the `focused` argument to the existing
   `ScrollableList::render` call, so the band dims under footer focus.
3. `handleInput()` gains the standard dispatch block **screen-wide**, placed once at the top right
   after the transient-`ACTIVATING` guard and before the per-state handling: if
   `handleInputWithDispatch` yields an action, recurse `handleInput(action)`; otherwise it was a
   focus toggle or hint navigation and is consumed. Because an unfocused footer consumes only
   SPACE, every direct key still falls through to its state handler, so the IDLE list nav and
   Enter-to-run and the CONFIRM Activate/Back direct keys are untouched.

Screen-wide placement (not IDLE-only) is deliberate and corrects the first on-device cut: SPACE
must reach the footer in every interactive state of a focusable-footer screen, or the operator
hits a dead SPACE on the pre-activation CONFIRM screen while it works on the IDLE list, which is
the very within-screen inconsistency this item exists to remove. The band exists only in IDLE, so
in the other states the highlighted footer badge is the focus cue. Transient ACTIVATING is
excluded by the guard (it swallows all input during the ~1.5 s enumeration wait); RUNNING and DONE
are single-action states included for one uniform SPACE behaviour rather than special-cased out,
since the dispatch block is already present and carving them out would add code to subtract
consistency.

**What existing code was considered.** `FooterHints::handleInputWithDispatch` (the shared dispatch
helper) and the scanner/sniffer consumers are the reference pattern, reused verbatim rather than
re-derived, including their top-of-handler placement. `ScrollableList::render`'s `focused`
parameter (added when the component took over band painting, slice-0063) is reused for the dim. No
new component or abstraction is introduced.

`mouse_jiggler` and `server_status` were considered for the same wiring and deliberately **not**
changed. The thing that justifies footer-focus machinery on a screen is a **selectable list**
whose band should visibly yield focus (the established idiom); badusb has one (the IDLE script
picker) and so earns the machinery, which is then applied screen-wide for consistency. Neither of
the other two draws a list: `server_status` is a single-action status readout, and while
`mouse_jiggler`'s CONFIRM is a two-action decision like badusb's, its actions are universal direct
keys (Activate = Enter, Back = Esc) with no list anywhere in the screen to anchor the idiom, so
introducing focus machinery there would be net-new ceremony for no band to dim, the kind the KISS
bar forbids. The ledger line that grouped all three is closed by this slice: badusb is wired, and
the two listless screens are recorded here as a conscious skip. If either later grows a selectable
list, that is the trigger to revisit, and this As-built is the record of why it was skipped.

## Verification

- `pio run -e cardputer` SUCCESS and fits. The change is device-only input/render wiring in
  `BadUsbScreen` (`#if defined(TARGET_CARDPUTER)` for the HID path; the screen is not
  host-constructible), so it carries no host test, as recorded for the slice-0069/0072 menu work;
  `pio test -e native` stays green (no pure/host-compiled logic changed).
- On-device DoD: in BadUSB IDLE confirm SPACE moves focus to the footer and dims the script-list
  band, `;`/`.` cycle the hints under focus, Enter fires the focused action and returns focus to
  the list, the unfocused direct-key path (list nav + Enter-to-run) is unchanged, and re-entering
  the screen starts unfocused. Confirm SPACE does nothing on `mouse_jiggler` and `server_status`.

## As built

Shipped confined to `badusb_screen.cpp` with no header change (the screen already owned a
`ui::FooterHints m_footer` and a `ScrollableList` script picker). `show()` seeds
`m_footer.setFocus(false)` so focus never carries across visits; `drawIdle()` passes
`!m_footer.hasFocus()` to the existing `ScrollableList::render` call so the band dims under footer
focus; and `handleInput()` runs the shared `handleInputWithDispatch` block once at the top, after
the transient-`ACTIVATING` guard, dispatching a focused hint by recursing `handleInput(action)`.

The dispatch was screen-wide from the shipped cut, but only after a correction: the first on-device
build wired it in the IDLE path alone, and the operator found that SPACE was dead on the
pre-activation CONFIRM screen while it worked on the list. Hoisting the one block to the top fixed
it, so SPACE now reaches the footer in every interactive state (CONFIRM/IDLE/RUNNING/DONE) and the
unfocused direct-key path is untouched everywhere. `mouse_jiggler` and `server_status` were left
unchanged by decision (no selectable list to anchor the idiom), closing the three-screen ledger
line; the Design section records why, so a future selectable list on either is the trigger to
revisit.

Measured: `pio run -e cardputer` SUCCESS, Flash 74.6%, 2,492,475 bytes (+296 vs the pre-slice
`8dd2b35`). `pio test -e native` 844/844 (device-only input/render wiring, not host-constructible,
so no host test, as recorded in Verification). On device the operator confirmed both DoD scenarios:
SPACE focuses the footer and cycles the hints on the pre-activation CONFIRM screen and on the IDLE
list (where the band also dims), Enter fires the focused hint, and the unfocused direct-key path
(list nav, Enter-to-run, Activate/Back) is unchanged.
