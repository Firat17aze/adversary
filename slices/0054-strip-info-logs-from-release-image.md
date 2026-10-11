---
id: '0054'
title: "Strip INFO logs from the release image via a release env"
type: slice
status: accepted
date: 2026-10-11
supersedes: []
superseded_by: []
---

## Goal

Bank the flash win ADR-0051 was for, in the shipped release artifact, without taking it out
of the operator's debug workflow. This closes the ledger's `[deferred] Lower the shipped
`ADV_LOG_LEVEL` to strip info/debug from the release image` item (ADR-0051, slice-0052 D2),
which was held open because `release.yml` builds the very same `-e cardputer` env a developer
flashes for on-device debugging — so dropping that env's level below INFO would remove the
`LOG_I` lifecycle output (start/stop, detect, "saved N") from live serial. The deferral named
its own exit: *"Flip it (or split a release env) once migration is broad enough that the
measured flash saving outweighs that visibility cost."* slice-0053 emptied the raw-`Serial`
ratchet, so **every** INFO/DEBUG string now routes through the compile-time gate and is
strippable; this slice takes the **split a release env** option so dev keeps INFO and only the
release artifact pays the quieter level.

The release-strip unblocks nothing further; the dead-`WardrivingExporter` `[bug]` remains its
own open ledger line.

## Definition of Done

- **Given** the shipped release pipeline
- **When** `release.yml` builds the firmware artifact
- **Then** it builds an env whose `ADV_LOG_LEVEL` is `ADV_LOG_WARN`, so the INFO (and
  already-stripped DEBUG) format strings are absent from the image's rodata and the artifact
  is measurably smaller than the INFO build

- **Given** a developer building or flashing the default `-e cardputer` env
- **When** they build
- **Then** `ADV_LOG_LEVEL` is still `ADV_LOG_INFO` and every `LOG_I` lifecycle line still
  prints on serial — the debug workflow is byte-for-byte unchanged

- **Given** the WARN release build
- **When** a converted path hits a `LOG_E`
- **Then** the error still formats and tees to the SD `ErrorLogger` — ERROR is above the gate,
  so fail-loud persistence survives the strip (only INFO was removed)

## Design

### D1 — A release env, not a flipped default

Add `[env:cardputer_release]` that `extends = env:cardputer` and overrides only the log level.
The inherited flags carry `-DADV_LOG_LEVEL=ADV_LOG_INFO` (from `env_embedded`), so the override
must *remove* that define, not merely add WARN after it. The working mechanism is
`build_unflags`, which edits the flag list before compile:

```ini
[env:cardputer_release]
extends = env:cardputer
build_unflags =
    ${env:cardputer.build_unflags}
    -DADV_LOG_LEVEL=ADV_LOG_INFO
build_flags =
    ${env:cardputer.build_flags}
    -DADV_LOG_LEVEL=ADV_LOG_WARN
```

`build_unflags` must re-list the parent's `-Werror=all` removal because redefining the option
replaces rather than appends under `extends`.

A `-U` cannot do this job here, though it reads like it should: PlatformIO/SCons routes `-U`
into `CCFLAGS` and `-D` into `CPPDEFINES`, which land in **different argument groups** on the
final gcc line — so `-UADV_LOG_LEVEL -DADV_LOG_LEVEL=ADV_LOG_WARN` in one `build_flags` list does
*not* produce `…-U… -DWARN` adjacently; the `-U` sits before both `-D`, INFO survives, and the
macro warns. This was measured (the `-U` build came out at the INFO size, 2 476 855 B, with a
`"ADV_LOG_LEVEL redefined"` warning) and rejected in favour of `build_unflags`.

`release.yml` changes its one build step from `pio run -e cardputer` to
`pio run -e cardputer_release` and its output dir from `.pio/build/cardputer` to
`.pio/build/cardputer_release`; the public artifact names (`adversary-cardputer-*`), the
factory-image merge, the checksums and the cache key are untouched (the artifact is still the
cardputer target; the cache key is a shared-toolchain label).

WARN, not ERROR, is the release level: DEBUG is already compiled out at the INFO default, so the
only thing WARN additionally strips is INFO; keeping WARN preserves recoverable-anomaly alerts
and keeps ERROR (with its SD tee) in the release image. The strip is therefore exactly "drop
INFO", which is what the ledger item asked for.

### D2 — Measured saving (the number the deferral wanted)

Built locally at the default `ADV_LOG_LEVEL`:

| cardputer build | Flash (bytes) | Partition (3 342 336 B) |
|---|---|---|
| INFO (`-e cardputer`, dev default) | 2 476 663 | 74.1% |
| WARN (`-e cardputer_release`, shipped) | 2 459 583 | 73.6% |
| **Saving** | **17 080 (~16.7 KiB)** | **0.51 pt** |

RAM is unchanged — format strings are flash rodata, not RAM. The win is modest but free of a
dev-visibility cost under this design, which is why the split-env form was chosen over flipping
the shared default.

### D3 — Considered and not reused

- **Flip `env_embedded`'s default to WARN** — rejected. One line, but it removes the operator's
  INFO lifecycle output from every flashed-for-debug build, the exact cost the deferral was
  protecting. The ~17 KiB does not outweigh that for the day-to-day dev build.
- **A release env for `m5stick`** — not added. `release.yml` ships only the cardputer artifact;
  m5stick has no release image to shrink, so a `m5stick_release` env would be speculative
  generality (global §3). m5stick stays at INFO.
- **A CI leg for `cardputer_release`** — not added. It differs from `cardputer` by one rodata-only
  `-D`; a dedicated ~5 min build leg buys near-zero coverage over the `cardputer` leg, and
  `release.yml` itself exercises the env on every release. The one-flag override is verified
  locally here instead (the size delta below).
- The existing `scripts/lint-raw-serial.mjs` ratchet and `test_log` suite are reused unchanged
  as the behavioural gate; this slice touches neither.

## Verification

- **Build sizes (the observable proof the strip took effect):**
  `pio run -e cardputer_release` builds green with Flash **2 459 583 B** (WARN); `pio run -e
  cardputer` still builds Flash **2 476 663 B** (INFO) — the 17 080 B delta *is* the stripped
  INFO rodata, and the unchanged INFO size proves the dev default was not disturbed (DoD
  scenarios 1 and 2). No `"ADV_LOG_LEVEL redefined"` warning in the release build (D1).
- **`pio run -e m5stick`** and **`pio test -e native`** (full suite, 814 cases) stay green — the
  shared `env_embedded`/native flags were not touched.
- **`test_log`** already pins the per-level gate boundaries (it overrides `ADV_LOG_LEVEL` per-TU
  and asserts that a level above the active one emits nothing): that suite is the unit proof
  that a WARN build drops `LOG_I`/`LOG_D` and keeps `LOG_W`/`LOG_E`, so no new per-level test is
  invented here (DoD scenario 3's macro-level truth; the SD tee itself is ESP32-only and was
  device-verified in slice-0052).
- **On-device (deferred, same as slice-0053):** confirming on real serial that a WARN release
  image emits no INFO lines while a forced failure still writes `/adversary/logs/error.log`
  needs hardware unavailable here; recorded in As built.

## As built

Shipped as designed, with one mechanism correction found during verification. `platformio.ini`
gained `[env:cardputer_release]` (`extends = env:cardputer`) that lowers the level to WARN via
`build_unflags` of the inherited `-DADV_LOG_LEVEL=ADV_LOG_INFO` plus a WARN `build_flags` define;
`release.yml` builds `-e cardputer_release` into `.pio/build/cardputer_release`, with the public
artifact names, factory merge, checksums and cache key unchanged. `env_embedded`, `m5stick` and
`native` were not touched, so the dev/debug `-e cardputer` default stays at INFO.

**Mechanism correction (D1):** the first cut used `-UADV_LOG_LEVEL -DADV_LOG_LEVEL=ADV_LOG_WARN`
in one `build_flags` list. It built at the **INFO** size (2 476 855 B) and emitted
`"ADV_LOG_LEVEL redefined"` — PlatformIO/SCons routes `-U` into `CCFLAGS` and `-D` into
`CPPDEFINES`, so the `-U` never sat between the two `-D` on the gcc line and INFO won. Switched
to `build_unflags` (string-removes the inherited INFO define before the WARN define is added),
which built at the WARN size with zero warnings. D1 now documents both.

**Measured (DoD scenarios 1 & 2 met):**

| cardputer build | Level | Flash | Partition |
|---|---|---|---|
| `-e cardputer` (dev default) | INFO | 2 476 663 B | 74.1% |
| `-e cardputer_release` (shipped) | WARN | 2 459 583 B | 73.6% |

The 17 080 B delta is the stripped INFO rodata; the INFO build is unchanged to the byte, so the
debug workflow is undisturbed. The release build emitted no `"redefined"` warning, and
`grep` of the verbose gcc lines showed every compile carrying only `-DADV_LOG_LEVEL=ADV_LOG_WARN`.

**Unaffected builds green:** `pio run -e m5stick` SUCCESS (2 421 719 B, unchanged); `pio test -e
native` **814/814**. DoD scenario 3 holds at the macro level — WARN ≥ ERROR, so `LOG_E` compiles
in and still tees to the SD `ErrorLogger`; `test_log` already asserts a WARN build keeps
`LOG_W`/`LOG_E` and drops `LOG_I`/`LOG_D`.

**Deviation — on-device check not performed (same as slice-0053).** Confirming on real serial
that a WARN release image prints no INFO lines while a forced failure still writes
`/adversary/logs/error.log` needs hardware unavailable in this environment. The SD-tee path is
unchanged from slice-0052 (device-verified there); this slice only changed which level compiles
in. Left as the lone open on-device confirmation.
