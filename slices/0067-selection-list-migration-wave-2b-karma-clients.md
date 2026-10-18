---
id: '0067'
title: "Selection-list migration wave 2b: the karma clients list"
type: slice
status: accepted
date: 2026-10-18
supersedes: []
superseded_by: []
---

## Goal

Continue the deferred ledger item **"Finish the selection-list migration"** (item 94). Wave 1
(slice-0065) routed the four simple single-list screens; wave 2a (slice-0066) did the two
BadBLE lists. This wave migrates the **karma clients list** (`KarmaScreenState::CLIENTS` in
`karma_screen.cpp`), the MED-HIGH-risk entry the 2026-10-18 survey flagged: it is the one
migration where the list selection also drives downstream state.

Unlike the wave-1/2a lists, this one is not a pure band-reconcile. It carries two genuine
behaviour changes the shared component brings: it currently paints **no scrollbar at all**
(so a clients list longer than the hardcoded 5 visible rows gives the operator no scroll
affordance), and its selected row uses the pre-slice-0063 band idiom (`BG_SECONDARY` fill +
`ACCENT` text). Routing it through `ScrollableList`/`ListScrollModel` adds the missing thumb,
reconciles the band to the house idiom, derives the viewport from the rect instead of a
hardcoded guess, and replaces clamp-at-ends with wrap, matching every already-migrated list.

Karma's *probes* and *credentials* lists are scroll-only logs (no selection), excluded here
and tracked by the ledger's scroll-only-component decision. The sniffer packet log is wave 2c.

## Definition of Done

- **Given** the karma clients list (`CLIENTS` state)
- **When** it renders
- **Then** the selected row is drawn by the shared component (`BG_SELECTED` when the list holds
  focus, `BG_SECONDARY` when the footer does), the per-row callback draws content only (MAC in
  `TEXT_PRIMARY`, the `(N pkts)` suffix in `TEXT_SECONDARY`) and never fills its own band, and
  the screen no longer hand-rolls a selection loop.

- **Given** more connected clients than fit the list rect
- **When** the clients list renders
- **Then** it paints the unified proportional scrollbar thumb (it painted none before), the
  visible-row count is whatever physically fits the rect at `theme::LIST_ROW_HEIGHT` (not the
  hardcoded `5`), and the selection never scrolls off-screen.

- **Given** the selection sits on the last client and the operator presses Down (or first
  client + Up)
- **When** the input is handled
- **Then** the selection wraps to the other end (a deliberate behaviour change: the list
  clamped before), matching every already-migrated list.

- **Given** a client is selected and the operator presses Enter
- **When** the CLIENT_DETAILS screen opens
- **Then** it shows the details for exactly the highlighted client, and CLIENT_TRAFFIC and the
  SD save continue to act on that same client: the index-to-MAC resolution still happens once,
  at Enter, and downstream state still keys off the captured MAC string, not the live index.

- **Given** the repository checks
- **When** the native suite and the cardputer build run
- **Then** native stays green (833/833) and the cardputer firmware compiles.

## Design

### Measured starting state (karma_screen.cpp, survey 2026-10-18)

| Aspect | Today | File:line |
|--------|-------|-----------|
| list source | `TrafficProxy::getClientMacList()` (`std::vector<std::string>`, re-fetched every frame) | `renderClients` ~820 |
| selection | `size_t selectedClientIdx_` | `.h` 116 |
| scroll offset | `size_t clientScrollPos_` | `.h` 115 |
| band idiom | `BG_SECONDARY` fill (`y-2`, `18` tall) + `ACCENT` text on selected; `TEXT_PRIMARY` otherwise | ~840-845 |
| row height | `18` (bare literal, `y += 18`) | ~856 |
| visible rows | `maxVisible = 5` (bare literal, not derived from rect) | ~827 |
| scrollbar | **none** (the correctness gap) | - |
| wrap | **clamps** (up: `idx > 0`; down: `idx < size-1`) | ~345, ~374 |
| footer focus | **wired** (`handleInputWithDispatch` at top of `handleInput`, all states) | 230-236 |
| downstream use | at Enter: `selectedClientMac_ = macs[selectedClientIdx_]`, then CLIENT_DETAILS | ~395-402 |

### The index-to-downstream-state analysis (the flagged risk)

`selectedClientIdx_` serves two roles: it is the list cursor, and at Enter it indexes the live
MAC vector once to snapshot `selectedClientMac_`. The two downstream states (CLIENT_DETAILS,
CLIENT_TRAFFIC) and the SD save all resolve the client fresh via
`getClientByMac(selectedClientMac_.c_str())`: they key off the **MAC string**, never the index.
So the index-to-MAC resolution is a single event at Enter, not a standing coupling. The
migration preserves that exactly: Enter reads `macs[clientList_.selectedIndex()]` under the
existing `idx < macs.size()` guard, and `ListScrollModel` re-clamps the index to the live count
every frame (`setItemCount` is fed from `getClientMacList().size()` in `renderClients`), so a
client disconnecting between frames cannot index past the end.

### Per-list changes

- Replace `size_t clientScrollPos_` + `size_t selectedClientIdx_` with a single
  `ListScrollModel clientList_` (trailing-underscore to match this file's member convention,
  not the `m_` prefix BadBLE uses). Keep `selectedClientMac_` (the downstream key).
- `renderClients`: keep the empty-state early return (`No clients connected`); replace the
  hand-rolled `for` loop + band fill with `ScrollableList::render(canvas, clientList_, 0,
  listTop, SCREEN_WIDTH, listH, theme::LIST_ROW_HEIGHT, cb, !footerHints_.hasFocus())`, feeding
  `setItemCount(macs.size())` each frame. `listTop = 22` (status bar 20 + 2px) and `listH =
  SCREEN_HEIGHT - FOOTER_HEIGHT - listTop`, matching slice-0066. The callback draws the MAC
  (`TEXT_PRIMARY`) then the `(N pkts)` suffix (`TEXT_SECONDARY`), fetching the per-row packet
  count via `getClientByMac(macs[idx])` as today; `macs` is captured by reference (the lambda
  runs synchronously inside `renderClients`, so the local vector outlives the call).
- Entering CLIENTS (the `c` key handler): `clientList_.setItemCount(...); setSelectedIndex(0)`
  replaces `clientScrollPos_ = 0; selectedClientIdx_ = 0`.
- Nav `;` (up) / `.` (down): `auto macs = getClientMacList(); if (!macs.empty()) {
  clientList_.setItemCount((int)macs.size()); clientList_.moveUp()/moveDown(); }`. Replaces the
  clamp branches; `moveUp`/`moveDown` wrap.
- Enter `\r`: `idx = clientList_.selectedIndex()`; keep `if (idx < macs.size()) {
  selectedClientMac_ = macs[idx]; screenState_ = CLIENT_DETAILS; }`.
- ctor: drop the `clientScrollPos_(0)` and `selectedClientIdx_(0)` initializers.

### Deliberate behaviour and visual changes (to confirm on device)

1. **Scrollbar appears** when the clients list exceeds the viewport (none drew before). This is
   the headline correctness gain, not a cosmetic one.
2. **Band reconciled** from `BG_SECONDARY` fill + `ACCENT` text to the house band (`BG_SELECTED`
   when focused, `BG_SECONDARY` when the footer holds focus), with content-coloured text. The
   MAC no longer turns `ACCENT` on selection; the `(N pkts)` suffix stays `TEXT_SECONDARY`.
3. **Row height 18 -> 14** (`theme::LIST_ROW_HEIGHT`); more clients fit, and the visible count
   is derived from the rect instead of the hardcoded `5`.
4. **Wrap** replaces clamp at both ends.

### Existing code considered

Reused the shared `ScrollableList`/`ListScrollModel` (slice-0062) and `theme::LIST_ROW_HEIGHT`
(slice-0064), same as waves 1/2a. Deliberately did **not** share a model with karma's probes or
credentials lists: those are scroll-only logs with no selection, a different shape tracked by
the ledger's scroll-only-component decision. Deliberately did **not** touch CLIENT_DETAILS or
CLIENT_TRAFFIC: neither is a selection list (CLIENT_TRAFFIC is itself a scroll-only log, also
out of scope). `selectedClientMac_` is kept verbatim as the downstream key; the index-to-MAC
resolution semantics at Enter are unchanged. No new abstraction introduced.

## Verification

Same shape as slices 0064-0066: a display-path refactor whose behaviour is pixel layout, which
no pure-logic unit test can assert, and the repo has no rendering-verification harness. The
`ListScrollModel` selection/scroll/wrap math is already unit-tested and is unchanged by this
slice, so the native suite guards that the shared contract did not regress; no new unit test is
warranted (asserting a screen calls `render` would test the implementation, not behaviour).

- **Native suite** - `pio test -e native` stays 833/833.
- **Cardputer build** - `pio run -e cardputer -j 4` compiles (`-j 4` is the host-RAM cap;
  default `-j 12` OOM-kills this ~5 GiB host).
- **On-device visual** - flash and, with >5 clients connected to the karma AP: the unified band
  on selection; the new scrollbar thumb + correct viewport; wrap at both ends; no clipping at
  the new row height; Enter opens details for the highlighted client and CLIENT_TRAFFIC + SD
  save act on that same client.

## As built

Shipped as designed. The karma clients list (`KarmaScreenState::CLIENTS`) now runs on the
shared component:

- `size_t clientScrollPos_` + `size_t selectedClientIdx_` replaced by one `ListScrollModel
  clientList_` (trailing-underscore to match this file's member convention); `selectedClientMac_`
  kept as the downstream key.
- `renderClients` keeps the empty-state early return and routes the populated list through
  `ScrollableList::render(..., listTop=22, listH=SCREEN_HEIGHT-FOOTER_HEIGHT-listTop,
  theme::LIST_ROW_HEIGHT, cb, !footerHints_.hasFocus())`, feeding `setItemCount(macs.size())`
  each frame. The callback draws the MAC (`TEXT_PRIMARY`) + the `(N pkts)` suffix
  (`TEXT_SECONDARY`), the per-row count still fetched via `getClientByMac`; `macs` captured by
  reference (the lambda runs synchronously inside the call). The hand-rolled loop, the
  `BG_SECONDARY`+`ACCENT` band, the bare `18` row height and the hardcoded `maxVisible = 5` are
  all gone.
- Entering CLIENTS (the `c` key) now seeds `clientList_.setItemCount(...)` + `setSelectedIndex(0)`.
  Nav `;`/`.` feed `setItemCount` then `moveUp`/`moveDown` (wrap) under an `!macs.empty()` guard.
  Enter reads `macs[clientList_.selectedIndex()]` under the existing `idx < macs.size()` guard,
  so the index-to-MAC snapshot semantics are unchanged.
- ctor dropped the `clientScrollPos_(0)` / `selectedClientIdx_(0)` initializers.

**Deliberate changes, confirmed on device:** the list gained the previously-absent scrollbar
thumb; the band is the house idiom (dim when the footer holds focus); the MAC no longer turns
`ACCENT` on selection; row height 18 -> 14 with the viewport derived from the rect; Up/Down wrap.
Enter opened details for the highlighted client and CLIENT_TRAFFIC + SD save acted on that same
client (the index-to-MAC resolution still happens once, at Enter).

**Scope honoured:** karma's three scroll-only log views (probes, credentials, client_traffic)
were left untouched (tracked by the scroll-only-component ledger item); the config form was left
hand-rolled (tracked by the config-form-unification ledger item). No new abstraction introduced.

**Verification run:** native `pio test -e native` 833/833; `pio run -e cardputer -j 4` SUCCESS
(Flash 74.6%, flat vs slice-0066); flashed to /dev/ttyACM0 (hash verified) and confirmed on
device ("looks correct").
