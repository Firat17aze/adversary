---
id: '0042'
title: "Live hardware/SD gating for root-menu list items"
type: slice
status: accepted
date: 2026-10-10
supersedes: []
superseded_by: []
---

## Goal

A root-menu **list** entry whose availability depends on hardware (RFID, GPS) or SD state
(Captures, BadBLE) must reflect the *current* state, so a peripheral attached after boot and
picked up by the Modules dashboard's Re-scan un-greys its menu entry without a reboot — the
same guarantee slice-0018 already gives the carousel tiles.

## Problem

`buildRootMenuItems()` freezes each gated entry into a `MenuItemType::ITEM_DISABLED` struct
at **build time**:

```cpp
// src/ui/menu_factory.cpp (RFID submenu)
adversary::ui::g_rfidDetected ?
    MenuItem::action("RFID Dashboard", ACTION_RFID_DASHBOARD, 'd') :
    MenuItem::disabled("RFID (Module missing)"),
```

The menu is built once at boot (`initializeMenu()` → `main.cpp`). Slice-0018 gave the
**carousel** `CarouselItem` a live `enabledFn` consulted every frame, so the Modules
dashboard's Re-scan (`redetectModules()`, which refreshes `g_rfidDetected` /
`g_gpsDetected`) un-greys the tile with no rebuild. But the Re-scan handler
(`modules_screen.cpp`) calls **only** `redetectModules()`, never `initializeMenu()` — so the
list layer never rebuilds. Hot-insert the RFID module, Re-scan: the carousel RFID tile lights
up, but entering it still shows the frozen `RFID (Module missing)` list entry, greyed and
unselectable. Same desync for the GPS-gated `Wardriving` entry.

The SD-gated list entries (`Captures`, `BadBLE`) are frozen the same way; today they only
un-grey via the full-menu rebuild that `Settings → Retry SD Mount` triggers
(`initializeMenu()`), not via the Modules Re-scan.

## Scope

**In:** a live `enabledFn` gate on `MenuItem`, mirroring `CarouselItem::enabledFn`, consulted
by selection and render so a gated list entry reflects current state every frame. Rewire the
four frozen entries (`RFID Dashboard`, `Wardriving`, `Captures`, `BadBLE`) to use it.

**Out:**
- **The SERVER carousel tile.** Its gate calls `SDManager::fileExists()` — real SD I/O that
  must not run per frame (see `menu_factory.cpp` and `module_detection.h`). It stays a
  build-time gate refreshed by the `Retry SD Mount` rebuild. The SD **list** entries, by
  contrast, gate only on the cached `SDManager::isReady()` bool, so they are safe to poll
  live — that is the distinction this slice relies on.
- **Rebuilding the menu on Re-scan.** The alternative fix — having Re-scan call
  `initializeMenu()` — resets the carousel cursor off MODULES (the exact reason slice-0018
  chose a live gate, not a rebuild). A live list gate avoids that.

## Definition of Done

- **Given** a `MenuItem` created with a live gate whose predicate currently returns false
- **When** the menu's selection logic is asked whether it is selectable
- **Then** it is not selectable (nav skips it, its shortcut does not fire) and it renders with
  the greyed "disabled" label — identical to a static `disabled()` item

- **Given** that same gated item, with the predicate now returning true, and **no** rebuild of
  the menu (no second `setItems`)
- **When** the menu's selection logic is asked again
- **Then** the item is now selectable and its shortcut fires its action — the gate is live, not
  frozen at build time

- **Given** an RFID module attached after boot and a Modules dashboard Re-scan
- **When** the operator opens the RFID submenu
- **Then** `RFID Dashboard` is selectable (not the greyed `RFID (Module missing)`), with no
  reboot

## Design

### Mirror the carousel's seam on `MenuItem`

`CarouselItem` already carries `std::function<bool()> enabledFn` and
`bool isEnabled() const { return enabledFn ? enabledFn() : enabled; }`. Give `MenuItem` the
same shape:

- `std::function<bool()> enabledFn;` — unset (empty) means "not gated".
- `std::string disabledLabel;` — the reason-bearing label shown greyed when the gate is off
  (`"RFID (Module missing)"`), preserving today's operator feedback. Empty for ungated items.
- `bool isEnabled() const { return enabledFn ? enabledFn() : type != MenuItemType::ITEM_DISABLED; }`
  — so a plain `disabled()` item (no `enabledFn`, `type == ITEM_DISABLED`) still reports
  `false`, and every other ungated item reports `true`. Fully backward compatible.
- A `MenuItem::gated(label, actionId, shortcut, disabledLabel, fn)` factory that builds a
  `type == ACTION` item carrying the gate — selectable only while the predicate holds.

### Selection and render consult the gate, not just the type

- `Menu::isSelectable()` becomes `(type is ACTION/SUBMENU/BACK) && item.isEnabled()`, so a
  gated-off entry is skipped by `navigateUp/Down`, `findNextSelectable`,
  `ensureValidSelection`, and the number/letter shortcut lookups — exactly as a
  `disabled()` item is today.
- `Menu::selectCurrent()` guards its `ACTION` branch on `isEnabled()`: a module unplugged
  while the cursor rests on its entry greys it live, and pressing ENTER must then do nothing
  rather than dispatch a dead action (fail loud — do not invoke an action the UI is showing
  as unavailable).
- `Menu::render()` chooses colour and label from `isEnabled()` instead of
  `type == ITEM_DISABLED`: a gated-off item draws in `TEXT_DISABLED` and shows
  `disabledLabel` when one is set, matching the frozen look it replaces.

### Rewire the four frozen entries

`buildRootMenuItems()` swaps the four boot-frozen ternaries for `MenuItem::gated(...)` with
cheap predicates. The predicates must **not** capture the function-local `sdManager` alias
(the `MenuItem` vector outlives `buildRootMenuItems`); they read the singletons/globals
directly:

- RFID: `[]{ return adversary::ui::g_rfidDetected; }`
- Wardriving: `[]{ return adversary::ui::g_gpsDetected; }`
- Captures / BadBLE: `[]{ return adversary::SDManager::getInstance().isReady(); }`

## Verification

- **Native:** a new `test_menu` adds `ui/components/menu.cpp` to the native build (its
  `render()` is `#ifdef ESP32`-guarded, so the selection/nav logic compiles off-target) and
  exercises the live gate through the public input surface: a `gated` item over a flag starts
  unselectable (its letter shortcut returns `false`, nav skips it), and after flipping the flag
  with **no** `setItems` rebuild its shortcut fires the action. The second assertion
  **fails before** this slice (a static `disabled()` item can never flip) and passes after —
  the required fails-before/passes-after regression. A static-`disabled()` case guards the
  backward-compatible `isEnabled()` default.
- **Build:** `pio run -e cardputer` links the new gate on both the list render and the
  factory.
- **On-device:** attach the RFID module after boot, Re-scan on the Modules dashboard, open the
  RFID submenu → `RFID Dashboard` is live and selectable with no reboot.

## As built

**Code (2026-10-10).** Implemented across five files:
- `menu.h` — `MenuItem` gains `std::function<bool()> enabledFn`, `std::string disabledLabel`,
  `isEnabled()` (gate wins; else `type != ITEM_DISABLED`, so ungated items are unchanged), and
  a `MenuItem::gated(label, actionId, shortcut, disabledLabel, fn)` factory. `render()` now
  drives colour and label off `isEnabled()` instead of `type == ITEM_DISABLED`, showing
  `disabledLabel` when a gate is off.
- `menu.cpp` — `isSelectable()` ANDs in `isEnabled()`, so nav/shortcuts skip a gated-off entry;
  `selectCurrent()`'s ACTION branch also guards on `isEnabled()` (refuse to dispatch an action
  the UI shows as unavailable).
- `menu_factory.cpp` — the four frozen ternaries (RFID Dashboard, Wardriving, Captures, BadBLE)
  become `MenuItem::gated(...)` with capture-less predicates reading the singletons/globals
  (`g_rfidDetected`, `g_gpsDetected`, `SDManager::getInstance().isReady()`); the now-unused
  `sdManager` local in `buildRootMenuItems()` was removed.
- `platformio.ini` — `ui/components/menu.cpp` added to the native `build_src_filter`
  (`render()` is `#ifdef ESP32`-guarded, so the selection/nav logic compiles off-target).
- `test/test_menu/test_menu.cpp` — new native suite.

**Verified in-conversation (2026-10-10):**
- `pio test -e native -f test_menu` → 3/3 pass.
- Fails-before proof: reverting only the `&& item.isEnabled()` clause in `isSelectable()`
  makes `test_gated_item_is_inert_while_gate_closed` fail (`Expected 1 Was 0` — the cursor
  lands on the gated item because a pre-slice ACTION is always selectable); restored → 3/3.
- `pio run -e cardputer` → SUCCESS. RAM 26.5% (86 704 B of 327 680), Flash 73.9% — within the
  no-PSRAM SRAM budget.

**On-device:** pending — attach the RFID module after boot, Re-scan on the Modules dashboard,
open the RFID submenu, confirm `RFID Dashboard` is live/selectable with no reboot. Tracked in
the LEDGER.
