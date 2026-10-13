---
id: '0058'
title: "Singletons are classified services; testability comes from extraction, not screen DI"
type: architecture
status: accepted
date: 2026-10-13
supersedes: []
superseded_by: []
---

# ADR-0058 — Singletons are classified services; testability comes from extraction, not screen DI

## Context

The ledger carried an architecture deferral: *"Document singleton usage and begin DI for
screens (unit-testability) — `getInstance()` call-site count has not dropped and a new
`UsbHidDevices` registrar was added."* Picking it up surfaced the data that the deferral
was written without, and that data breaks half of its premise.

Measured 2026-10-13 (`grep` over `src/`):

- **572** `getInstance()` call sites. **341 (60%) sit inside `ui/screens/`.**
- The distribution is dominated by a handful: `SettingsManager` **94**, `EventBus` **69**,
  `ToastManager` **68**, `SDManager` **44**, `CaptureRegistry` **40**, then a long tail
  (`BLESpanner` 35, `TrafficProxy` 25, …).

Two facts about the build decide the rest:

- The native test build (`platformio.ini` `[env:native]` `build_src_filter`) is an
  **allowlist**: `-<ui/>` excludes **every** screen, and exactly four *display-free* files
  are added back — `ui/components/pulse_strip.cpp`, `ui/menu_routes.cpp`,
  `ui/components/menu.cpp`, and `hal/input/input_keymap.cpp`.
- A screen `.cpp` draws through M5GFX / M5Unified and **cannot link natively**, with or
  without its dependencies injected.

The deferral's stated goal was **unit-testability**. But injecting singletons into a screen
cannot make that screen unit-testable: the screen TU never compiles off-device no matter how
its dependencies arrive. The 341 in-screen `getInstance()` calls are real coupling, yet DI
does not convert them into test payoff — it would touch ~341 call sites to thread ~15
services through screen constructors and leave every screen exactly as untestable as before.

The pattern that **has** delivered screen-logic testability in this repo is the inverse:
lift the pure, display-free computation **out** of the screen into its own TU and allowlist
it into the native build. That is exactly what slices 0029 (`menu_routes`), 0030
(`input_keymap`), and 0024/0031 (`menu`) did — each produced a regression test that fails
before and passes after. Extract the pure core; do not inject dependencies into the impure
shell.

No prior ADR covers singletons or dependency injection, so this introduces the policy rather
than revising one.

## Decision

1. **Singletons are classified, not uniformly condemned.** Two kinds, and only one is a
   smell:
   - **(a) Legitimately-global services** — one instance by nature, cross-cutting, with no
     sensible per-caller identity: `EventBus` (a message bus *is* global), `TimeManager`
     (the wall clock), `ThemeManager` and `ToastManager` (process-wide UI services).
     `getInstance()` at their call sites is acceptable; they are the composition's ambient
     services, not coupling to remove.
   - **(b) Stateful managers that are singletons for convenience** — `SettingsManager`,
     `SDManager`, `CaptureRegistry`, and the feature-module managers. These own real state
     and hardware; their global access is the coupling, and it is that coupling which blocks
     testing the logic built on top of them.

2. **Testability comes from extraction, not injection-into-screens.** The unit-test lever
   for screen logic is to lift the pure, display-free computation out of the screen into a
   natively-compiled TU (the 0029–0031 pattern) and test it there directly. This is the
   standard move and the precedent to follow whenever a screen accumulates logic worth
   asserting.

3. **Dependency injection is the right tool only for natively-compiled non-screen classes**
   (`modules/`, `core/`, `utils/`) that bury a category-(b) `getInstance()` inside otherwise
   pure logic. There, taking the dependency as a parameter lets a test pass a fake and earns
   a genuine regression test. DI is applied **case by case**, when a class has testable logic
   and the seam pays for itself — never as a blanket sweep (rule of three; the wrong
   abstraction costs more than the duplication it removes).

4. **"DI for screens for unit-testability" is rejected.** No amount of constructor injection
   makes a `-<ui/>`-excluded, M5GFX-bound screen natively testable; the change would touch
   ~341 call sites for zero test payoff. The goal it was meant to serve is met by decision 2.

5. **Going-forward hygiene, not a retrofit.** New leaf logic should not reach for a
   category-(b) `getInstance()`: prefer passing the dependency, or keep the logic pure and
   let the impure caller supply its inputs. This governs new code and any TU extracted under
   decision 2 — it is **not** a mandate to rewrite the existing 572 sites.

## Consequences

- The ledger's "document singletons / begin DI for screens" deferral closes: the inventory
  and classification live here, and the DI-for-screens half is settled as rejected with the
  data that rejects it. Its line is deleted when this ADR lands.
- **No large refactor is authorized or implied.** The 572 existing call sites stay as they
  are; this ADR changes what *new* code does and names the extraction pattern for when a
  screen grows testable logic.
- A future DI or parameterization of a specific category-(b) dependency is a normal slice
  citing this ADR; it needs no new decision — unless it reverses decision 4 (literal screen
  DI), which would require an ADR superseding this one.
- Review gains a shared vocabulary: introducing a **new** category-(b) singleton is now a
  visible choice to justify, not a silent default, and "make this testable" has one correct
  answer (extract the core) instead of a tempting wrong one (inject into the shell).
