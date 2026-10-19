---
id: '0076'
title: "Make the evil-twin clients list show every connected client"
type: slice
status: accepted
date: 2026-10-19
supersedes: []
superseded_by: []
---

## Goal

Close the ledger bug "Evil Twin clients list is dead/broken" (found by the slice-0066
survey 2026-10-18, pre-existing). `EvilTwinScreen::renderClients` draws the connected-client
list from `y=28` at a 14px pitch and stops at `y < SCREEN_HEIGHT - 30`, so only 6 rows fit;
the AP is configured for up to 8 clients, so a full AP silently drops 2. The screen also
carries a `clientScrollPos_` field that is dead three ways: decremented by the UP handler,
never incremented (no DOWN branch for the CLIENTS view), and never read by `renderClients`,
which always iterates from index 0.

The fix migrates the clients view onto the shared `ScrollableList` + `ListScrollModel`, the
same read-only scroll idiom the wardriving network log runs on (slice-0074): a selection band
that wraps at the ends, an auto-derived visible-row count, and the unified scrollbar. This was
chosen (operator, 2026-10-19) over a bespoke "shrink the pitch so all 8 fit" layout: fitting
all 8 only works while the cap stays 8 and the pitch stays tight, so it re-breaks the moment
the cap rises toward the SDK's ceiling of 10 or the pitch returns to the house 14px, and it
makes this one list a snowflake against the idiom just established across the app. The clients
view is read-only like the wardriving log (no per-row ENTER action); the future target-picker
feature (deauth/kick a *selected* client) is logged to the ledger and will simply attach an
action to the row this slice makes selectable.

## Definition of Done

- **Given** an evil-twin AP with more connected clients than fit on one screen (up to the cap
  of 8)
- **When** the operator opens the CLIENTS view and presses DOWN past the last visible row
- **Then** the list scrolls so every connected client is reachable, with the shared scrollbar
  showing position, and none is silently dropped

- **Given** the CLIENTS view
- **When** the operator presses UP at the top row or DOWN at the bottom row
- **Then** the selection wraps to the other end, matching the wardriving network log and BLE
  scanner lists (one navigation idiom across the app)

- **Given** the clients view renders
- **When** its rows are drawn
- **Then** they are drawn through `ScrollableList` + a `ListScrollModel` member at
  `theme::LIST_ROW_HEIGHT`, with no bespoke per-row layout and no dead `clientScrollPos_`
  field remaining

- **Given** the SoftAP client cap and the display read-back array
- **When** either is sized in code
- **Then** both derive from one named constant (`attack::EVIL_TWIN_MAX_CLIENTS`), so the
  read-back buffer can never be smaller than what the AP admits

## Design

**Reuse the shared scroll idiom.** The clients view renders through `ScrollableList::render`
against a `ListScrollModel clientList_` member, exactly as `WardrivingScreen::drawNetworkLog`
does (slice-0074): `itemHeight = theme::LIST_ROW_HEIGHT`, the list band spanning from the
status bar to the footer, the component deriving the visible-row count and keeping the
selection on screen, and `moveUp`/`moveDown` wrapping at the ends. The clients are read fresh
each frame with `getClients()` into a local `ap::APClient[EVIL_TWIN_MAX_CLIENTS]`; the render
callback paints one row (MAC + RSSI) from that array, which stays in scope for the duration of
the synchronous `ScrollableList::render` call. `clientList_.setItemCount(clientCount)` each
frame keeps the model's bounds live, so the navigation handlers only call `moveUp`/`moveDown`
(the same split the wardriving log uses, where render owns the count).

**Read-only, like the wardriving log.** The clients list has no ENTER action: the wardriving
network log is the same shape (selection band + wrap, no per-row action), so a band here is the
house idiom, not a promise of an action that does not exist. The one thing that would justify a
per-client action (deauth/kick a selected client) is the target-picker feature logged to the
ledger; it will attach an ENTER handler to the row this slice already makes selectable, so this
migration is its groundwork rather than throwaway.

**Name the cap once (rule of three).** The literal `8` appears at three sites that must agree:
the SoftAP `max_connection` (`evil_twin.cpp`), the display read-back array, and the
`getClients(clients, 8)` call (`evil_twin_screen.cpp`). They are the same quantity (how many
clients the AP admits), so a drift between them truncates the read-back. Named once as
`constexpr uint8_t attack::EVIL_TWIN_MAX_CLIENTS = 8` in `evil_twin.h` and used at all three.
This is distinct from `config::MAX_CLIENTS` (100), which bounds other modules' client tracking,
not the evil-twin SoftAP, and from the ESP32 SDK's own SoftAP ceiling of 10.

**Replace the dead scroll state.** `clientScrollPos_` (declared in `evil_twin_screen.h`), its
reset at the `l`/`L` CLIENTS-entry handler, and its orphaned decrement in the UP (`;`) handler
are removed, superseded by the live `clientList_` model: UP/DOWN now move a real selection
through the model, and entry resets it to the top. The credentials view's own
`credentialScrollPos_` is untouched (different view, out of scope).

**What existing code was considered.** `ScrollableList` + `ListScrollModel` (ble_scanner,
wardriving log, slice-0062/0074) is reused as the established list idiom; the wardriving log's
read-only variant is mirrored directly. A bespoke "shrink the row pitch so all 8 fit without
scrolling" layout was considered and rejected: it is a lone snowflake against that idiom and
re-breaks if the cap rises toward the SDK's 10 or the pitch returns to 14px, so it trades a
real recurrence risk for a small amount of saved code. The sibling credentials view's local
`scrollPos + ^/v` idiom was also considered and not reused: it is the pre-component pattern the
app is migrating away from, so copying it would spread the thing being retired.

## Verification

- `pio run -e cardputer` SUCCESS and fits: the change wires the device-only evil-twin screen
  onto the shared `ScrollableList`, plus a shared constant read by the module. `evil_twin_screen.cpp`
  is outside the native `build_src_filter` (`-<ui/>`), so it carries no host test, as recorded
  for the slice-0069/0072/0073/0074/0075 UI work.
- `pio test -e native` stays green (844/844): `getClients` is `#ifndef UNIT_TEST`-guarded and
  unchanged in behavior; the named constant and the screen-side migration touch no
  host-compiled logic.
- `node scripts/lint-docs.mjs` 0 errors.
- On-device DoD (the scroll/selection behavior the host cannot assert, verified by eye on the
  Cardputer): with more clients connected than fit on screen, DOWN scrolls to reach them all
  with the scrollbar tracking position; UP at the top and DOWN at the bottom wrap; no client is
  silently dropped.

## As built

The evil-twin CLIENTS view now runs on the shared `ScrollableList` + a `ListScrollModel
clientList_` member, the same read-only scroll idiom as the wardriving network log: 14px rows
(`theme::LIST_ROW_HEIGHT`), the component deriving the visible-row count over the status-bar-to-
footer band, keeping the selection on screen, and drawing the unified scrollbar. Each frame the
clients are read fresh with `getClients()` into a local `ap::APClient[EVIL_TWIN_MAX_CLIENTS]`
buffer that the render callback paints from; `clientList_.setItemCount(clientCount)` keeps the
model's bounds live. UP/DOWN call `moveUp`/`moveDown` (wrapping at the ends), entering the view
resets the selection to the top, and the dead `clientScrollPos_` field with its orphaned UP
decrement is gone. There is no ENTER action, matching the wardriving log; the target-picker
feature (deauth/kick a selected client) is logged to the ledger and will attach an action to
this now-selectable row.

The SoftAP client cap is named once as `constexpr uint8_t attack::EVIL_TWIN_MAX_CLIENTS = 8` in
`evil_twin.h` and used at the three sites that must agree: the `WiFi.softAP(..., max_connection)`
call, the display read-back array, and the `getClients()` request, so they can no longer drift
and truncate. The bespoke "shrink the pitch so all 8 fit without scrolling" layout from the
first cut was dropped in favor of the shared component: fit-all is a lone snowflake against the
idiom and re-breaks if the cap rises toward the SDK's 10 or the pitch returns to 14px, whereas
the component scales to any count.

Measured: `pio run -e cardputer` SUCCESS, Flash 74.6%, 2,493,391 bytes. `pio test -e native`
844/844 (no host-compiled logic changed; `evil_twin_screen.cpp` is outside the native
`build_src_filter`, and `getClients` is `#ifndef UNIT_TEST`-guarded and unchanged). `scripts/
lint-docs.mjs` 0/0. On device 2026-10-19 the operator confirmed that with more clients than fit
on screen DOWN scrolls to reach them all with the scrollbar tracking position, UP/DOWN wrap at
the ends, and no client is dropped.
