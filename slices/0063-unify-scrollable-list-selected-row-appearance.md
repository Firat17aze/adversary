---
id: '0063'
title: "Unify the ScrollableList selected-row appearance (component owns the band)"
type: slice
status: accepted
date: 2026-10-18
supersedes: []
superseded_by: []
---

## Goal

The scrollable-list migration (slice-0062) routed all 15 selection-list call sites through
one `ScrollableList::render`, but deliberately preserved each screen's hand-rolled
selected-row appearance verbatim, to unify it once at the end. This is that end. A survey of
the 15 sites (2026-10-18) found the selection highlight had drifted into five band idioms
and three text idioms: band fills of `BG_SELECTED`, `BG_SECONDARY`, and a bright `ACCENT`
bar; selected text of `TEXT_PRIMARY`, `ACCENT`, and `TEXT_INVERSE`; and footer-focus handled
three different ways (suppress the band, dim it, or ignore focus). The operator raised this
after the rfid migration: the audit-summary and write-file lists "work well, but the item
highlight color does not seem unified with the rest of the screens."

This slice makes the **component** own the selection band, so a single rule produces it
everywhere, and converges the text idioms to one policy. It fixes the root cause (15 private
copies of the highlight) rather than retokening each site, which would leave the next list to
drift again.

Operator decisions recorded 2026-10-18 (AskUserQuestion):
- **Mechanism**: the component owns the band (not a shared color token each site still draws).
- **Scope**: colour only. Row-height unification is split out and deferred (see the ledger
  "Named `ScrollableList` row-height tokens" item); shrinking rows changes legibility and
  visible-row counts and wants its own measured slice.
- **Text policy**: band-only for data lists (text encodes content/role, never selection), but
  the config/edit screens keep their bright-`ACCENT` selected text as an intentional "you are
  editing this field" affordance.

## Definition of Done

- **Given** any selection list rendered through `ScrollableList::render` with list focus
- **When** a row is the selected row
- **Then** the component paints that row's band as `theme::BG_SELECTED()` across the full row
  rect, and no screen callback paints a band of its own.

- **Given** a selection list whose screen has a focusable footer, and the footer holds focus
- **When** the list is drawn
- **Then** the selected row's band is dimmed to `theme::BG_SECONDARY()` (one unified
  "unfocused" look), replacing the old mix of suppress-the-band and dim-the-band.

- **Given** a data list (every list except the four config screens)
- **When** its rows are drawn
- **Then** the main content line is `theme::TEXT_PRIMARY()` and secondary/metadata lines are
  `theme::TEXT_SECONDARY()` regardless of selection, semantic colours (RSSI, status, badges)
  are kept, and no row uses `ACCENT` or `TEXT_INVERSE` text to signal selection.

- **Given** a config screen (deauth, probe-flood, beacon-spam, ble-spam)
- **When** a config field is the selected row
- **Then** its label and value stay `theme::ACCENT()` (the edit-field cue) over the
  component-drawn band, and unselected fields are `theme::TEXT_PRIMARY()`.

- **Given** the full build
- **When** `pio test -e native` and `pio run -e cardputer` run
- **Then** both are green, and on device every list screen shows the same band and the policy
  above, confirmed visually.

## Design

### What exists and is reused

`ScrollableList::render` (`src/ui/components/scrollable_list.h`, slice-0062) already loops the
visible rows and calls a per-row `drawRow(canvas, itemIndex, rowTopY, isSelected)` callback,
then draws the one unified scrollbar thumb. It is the single chokepoint every one of the 15
sites now passes through (verified: `grep ScrollableList::render src/ui/screens` lists all 12
screen files). So the band belongs here: adding it to the loop converges all 15 sites with no
new abstraction. `ListScrollModel` is untouched (selection/scroll math, already unit-tested).
The theme tokens (`BG_SELECTED`, `BG_SECONDARY`, `TEXT_PRIMARY`, `TEXT_SECONDARY`, `ACCENT`)
are reused as-is; no new token is introduced (that is the deferred row-height item's concern,
not this one).

Considered and rejected: a shared `drawSelectedBand()` helper the callbacks still call. That
keeps the band drawable (and mis-drawable) at each site, so it does not fix the drift root
cause; the component owning the loop is the only place the band is drawn exactly once.

### Component change

`render` gains a trailing defaulted parameter `bool focused = true` (legal after the
template-deduced `DrawRow`; existing callers compile unchanged). Before invoking the callback
for the selected row, the component fills the band across the clean row rect:

```cpp
const uint16_t bandColor = focused ? theme::BG_SELECTED() : theme::BG_SECONDARY();
for (int row = 0; ...) {
    const int itemIndex = offset + row;
    const int16_t rowTopY = y + static_cast<int16_t>(row * rowHeight);
    const bool isSelected = (itemIndex == selected);
    if (isSelected) {
        canvas.fillRect(x, rowTopY, w, rowHeight, bandColor);
    }
    drawRow(canvas, itemIndex, rowTopY, isSelected);
}
```

The thumb still draws last, over the band, as today. The file's doc comment is reversed: it
currently says "row appearance stays with the screen: selection highlight, columns, colours";
it will say the component paints the selection band and the callback draws content only.

Screens whose only input into "focused" is a footer pass `!footerHints_.hasFocus()`; all
others keep the `true` default. This unifies the three footer-focus behaviours onto one:
dim-to-`BG_SECONDARY`.

### Per-site plan (15 call sites)

Every callback loses its own band `fillRect`. Text/footer deltas (V = visible change to
verify on device):

| Site | Text before -> after | Footer | Band before -> after |
|---|---|---|---|
| wardriving drawNetworkLog | main PRIMARY/SECONDARY -> PRIMARY both; RSSI colour kept | default | BG_SELECTED (y-1) -> component |
| scanner drawNetworkEntry | SSID PRIMARY both (kill dead ternary `:552`); meta SECONDARY/DISABLED -> SECONDARY both (V); bars PRIMARY/ACCENT -> ACCENT both (V); hidden-SSID variant -> one muted colour (V) | default | BG_SELECTED (ENTRY_HEIGHT-1) -> component |
| whitelist drawList | main PRIMARY/SECONDARY -> PRIMARY both | default | BG_SELECTED -> component |
| captures drawFileList | main PRIMARY both (no change) | suppress -> `!hasFocus()` dim (V) | BG_SELECTED -> component |
| radio drawMain | main PRIMARY/SECONDARY -> PRIMARY both | default | BG_SELECTED -> component |
| radio drawSignalList | main PRIMARY/SECONDARY -> PRIMARY both | default | BG_SELECTED -> component |
| radio drawFskSelect | main PRIMARY/SECONDARY -> PRIMARY both | default | BG_SELECTED -> component |
| handshake drawHandshakeList | SSID PRIMARY/SECONDARY -> PRIMARY both; brackets PRIMARY/SECONDARY -> PRIMARY both; badge type colour kept | default | **ACCENT bar -> BG_SELECTED (V, large)** |
| rfid drawSectorListItem | main PRIMARY both; **drop the ACCENT ">" cursor + extra key column** (band now signals selection); key labels SECONDARY both; +/- status kept | default | BG_SECONDARY (inset x=4) -> component BG_SELECTED, full rect (V) |
| rfid drawWritePick | filename ACCENT/PRIMARY -> PRIMARY both (V; it is a data list, not a config field) | default | BG_SECONDARY -> BG_SELECTED (V) |
| deauth drawConfig | label+value ACCENT/PRIMARY kept | dim -> `!hasFocus()` (same look) | BG_SELECTED/BG_SECONDARY -> component |
| probe-flood drawConfig | label+value ACCENT/PRIMARY kept | dim -> `!hasFocus()` (same look) | BG_SELECTED/BG_SECONDARY -> component |
| beacon-spam drawConfig | label+value ACCENT/PRIMARY kept | dim -> `!hasFocus()` (same look) | BG_SELECTED/BG_SECONDARY -> component |
| ble-spam drawConfig | label+value ACCENT/PRIMARY kept | verify footer focusable; pass flag if so | BG_SECONDARY -> BG_SELECTED (V) |
| ble-scanner drawList | name TEXT_INVERSE/PRIMARY -> PRIMARY both (V); MAC INVERSE/SECONDARY -> SECONDARY both (V); bar INVERSE/ACCENT -> ACCENT both (V) | suppress -> `!hasFocus()` dim (V) | BG_SELECTED -> component |

### Watch items (on-device verification)

- **Band y-offset churn.** Several sites drew the band offset from the row top (wardriving
  y-1, radio y-2, scanner ENTRY_HEIGHT-1, rfid inset x=4). The component draws it at the
  exact row rect `(x, rowTopY, w, rowHeight)`. Row *content* is positioned by each callback's
  own `setCursor`, independent of the old band, so it should not move; but a 1-2 px text nudge
  per site may be needed if a row looks off. Verify each list on device.
- **ble-spam footer.** The survey found it ignores footer focus. Confirm whether its footer is
  focusable; if yes, pass `!footerHints_.hasFocus()` for consistency, else leave the default.
- **Subtle band.** `BG_SELECTED` is a dark-red band; band-only data lists now rely on it as
  the sole cue. If it reads too weak on device, that is a single theme-token brightness
  decision (one place), not a per-site regression; do not re-scatter text recolouring to
  compensate.

## Verification

**Component band logic — reasoned at the chokepoint.** The band rule lives in one place
(`ScrollableList::render`), so correctness is "the selected row, and only it, gets a band of
the right colour." `ScrollableList::render` touches `Canvas` and `theme`, so like the other
UI-screen renderers it is outside the native build (the established ESP32-guarded convention,
slice-0062) and cannot be unit-asserted; `ListScrollModel`'s selection/offset math (which
decides *which* index is selected and visible) is already covered by the native
`test_list_scroll_model` suite and is unchanged here, so no model test is added or needed.

**Appearance — on device**, against the Definition-of-Done scenarios, across every list
screen (wardriving, scanner, whitelist, captures, radio main/signal/fsk, handshake, rfid
audit-summary + sector-detail + write-pick, deauth, probe-flood, beacon-spam, ble-spam,
ble-scanner):
- the selected band is the same `BG_SELECTED` colour and full-row rect on every screen;
- data-list rows show no `ACCENT`/`TEXT_INVERSE` selection text; config fields still redden
  when selected;
- on captures and ble-scanner, giving the footer focus now dims the band (it no longer
  vanishes), and the config trio looks unchanged;
- handshake's bright-red bar is gone, replaced by the standard band; rfid's ">" cursor is
  gone, the band signals selection;
- each row's text still sits correctly inside the band (the y-offset watch item).

**Builds**: `pio test -e native` (833 tests) and `pio run -e cardputer` both green before
merge; environments built serially.

## As built

Shipped as designed. `ScrollableList::render` gained `bool focused = true` and now paints the
selected row's band itself (`BG_SELECTED` focused / `BG_SECONDARY` dim) across the full row
rect, before the content callback; the header doc comment was reversed to say so. All 15 call
sites dropped their own band fill:

- **Data lists** (wardriving, scanner, whitelist, captures, radio main/signal/fsk, handshake,
  rfid audit-summary + write-pick, ble-scanner) now use band-only text: main line
  `TEXT_PRIMARY`, secondary `TEXT_SECONDARY`, semantic colours (RSSI, status dots, badges)
  kept, no selection-driven recolour. The dead `selected ? TEXT_PRIMARY : TEXT_PRIMARY`
  ternary at scanner was removed; handshake's bright `ACCENT` bar and rfid's `">"` cursor plus
  its selected-only key-name column are gone (the cracked key stays on the sector-detail
  screen); `drawSectorListItem` lost its now-unused `selected` parameter (header + definition);
  ble-scanner's `TEXT_INVERSE` text became `TEXT_PRIMARY`.
- **Config screens** (deauth, probe-flood, beacon-spam, ble-spam) kept their `ACCENT`
  selected-field text; their band now comes from the component.
- **Footer-focus** unified on dim: captures, ble-scanner, deauth, probe-flood, beacon-spam and
  ble-spam pass `!footerHints_.hasFocus()`; the two that used to suppress the band now dim it.

Each Definition-of-Done scenario was confirmed on device (Cardputer ADV): one `BG_SELECTED`
band on every list, data-list rows with no `ACCENT`/`INVERSE` selection text, config fields
still reddening, captures/ble-scanner footer-focus dimming instead of vanishing, handshake and
rfid outliers converged.

One deviation from the plan, caught on device and fixed: the band y-offset watch item fired on
the **radio** screens only. Radio alone drew its old band at `rowY - 2` while placing text at
`rowY`, so with the component drawing the band at the exact row rect the text looked top-
aligned. Fixed by nudging the three radio callbacks' text to `rowY + 2`; no other screen needed
a text adjustment. `ble-spam`'s footer was confirmed focusable (it dispatches footer input), so
it passes the focus flag like the other config screens.

Verification: `pio run -e cardputer` SUCCESS and `pio test -e native` 833/833 (the unchanged
`test_list_scroll_model` among them); flashed to `/dev/ttyACM0` and visually confirmed across
every list screen. (Note: the default `-j` build OOM-killed on this host's free RAM; `-j 4`
builds cleanly, a host constraint unrelated to the change.)
