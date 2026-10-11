#!/usr/bin/env node
// Forbids NEW raw `Serial.print` / `println` / `printf` call sites outside the sanctioned
// logging sinks, so the raw-Serial sprawl cannot grow while the 583 legacy sites migrate
// behind the project logging layer (ADR-0051, slice-0052).
//
//   node scripts/lint-raw-serial.mjs [dir ...]      (default: src)
//
// Zero dependencies by design, like the other scripts/*.mjs. Exit 1 on any finding.
//
// Ratchet model (file-set, not per-site counts): a JSON baseline lists the legacy files
// still permitted to contain raw Serial. The lint fails when
//   (a) a file NOT in the baseline contains raw Serial  -> a new site, blocked; or
//   (b) a baselined file contains NO raw Serial          -> a stale entry a migration left
//                                                           behind; delete it. The baseline
//                                                           may only ever shrink.
// File granularity (not counts) is deliberate: a migration converts a file wholesale, so it
// leaves the baseline entirely, and a per-site count would fire spurious CI failures every
// time an unrelated edit split or joined a printf across 60-odd active files. The residual
// gap -- adding a raw line to an already-legacy file passes -- is caught at review and closes
// as the baseline shrinks to empty.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join, extname, relative } from 'node:path';

const SOURCE_EXTS = new Set(['.c', '.cc', '.cpp', '.cxx', '.h', '.hpp', '.hxx']);

// The only files allowed to call Serial.print* directly: the logging layer and its SD sink.
// Every other site must route through LOG_E/W/I/D (ADR-0051).
const SANCTIONED_SINKS = new Set(['src/utils/log.h', 'src/utils/error_logger.h']);

const BASELINE_PATH = 'scripts/raw-serial-baseline.json';

// One raw-Serial call: Serial.print / Serial.println / Serial.printf, tolerating whitespace.
const RAW_SERIAL = /\bSerial\s*\.\s*print(?:ln|f)?\s*\(/g;

/** Scan one file's text; return findings with 1-based line/column. Exported for the test. */
export function scanText(text) {
  const findings = [];
  const lineStarts = [0];
  for (let i = 0; i < text.length; i++) {
    if (text[i] === '\n') lineStarts.push(i + 1);
  }
  const locate = (index) => {
    let lo = 0;
    let hi = lineStarts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (lineStarts[mid] <= index) lo = mid;
      else hi = mid - 1;
    }
    return { line: lo + 1, col: index - lineStarts[lo] + 1 };
  };
  for (const match of text.matchAll(RAW_SERIAL)) {
    findings.push(locate(match.index));
  }
  return findings;
}

/**
 * Decide pass/fail from a map of {relPath -> hasRawSerial} and the baseline set.
 * Returns { newSites, staleEntries } — both empty means pass. Exported for the test.
 */
export function evaluateBaseline(hasRawByFile, baseline) {
  const newSites = [];
  for (const [rel, has] of hasRawByFile) {
    if (has && !baseline.has(rel) && !SANCTIONED_SINKS.has(rel)) newSites.push(rel);
  }
  const staleEntries = [];
  for (const rel of baseline) {
    if (!hasRawByFile.get(rel)) staleEntries.push(rel);
  }
  return { newSites, staleEntries };
}

function collectSources(dir) {
  const out = [];
  for (const entry of readdirSync(dir)) {
    const path = join(dir, entry);
    if (statSync(path).isDirectory()) out.push(...collectSources(path));
    else if (SOURCE_EXTS.has(extname(entry))) out.push(path);
  }
  return out;
}

function main(argv) {
  const dirs = argv.length > 0 ? argv : ['src'];
  let baseline;
  try {
    baseline = new Set(JSON.parse(readFileSync(BASELINE_PATH, 'utf8')));
  } catch (err) {
    console.error(`lint-raw-serial: cannot read ${BASELINE_PATH}: ${err.message}`);
    return 1;
  }

  const hasRawByFile = new Map();
  const findingsByFile = new Map();
  for (const dir of dirs) {
    for (const file of collectSources(dir)) {
      const rel = relative('.', file);
      if (SANCTIONED_SINKS.has(rel)) continue;
      const findings = scanText(readFileSync(file, 'utf8'));
      hasRawByFile.set(rel, findings.length > 0);
      if (findings.length > 0) findingsByFile.set(rel, findings);
    }
  }

  const { newSites, staleEntries } = evaluateBaseline(hasRawByFile, baseline);

  for (const rel of newSites) {
    for (const f of findingsByFile.get(rel)) {
      console.error(`${rel}:${f.line}:${f.col}: new raw Serial.print* outside the logging layer`);
    }
  }
  for (const rel of staleEntries) {
    console.error(`${rel}: baseline entry is stale (no raw Serial.print* left) — remove it from ${BASELINE_PATH}`);
  }

  if (newSites.length > 0 || staleEntries.length > 0) {
    console.error(
      `\nlint-raw-serial: ${newSites.length} file(s) with new raw Serial, ` +
      `${staleEntries.length} stale baseline entr${staleEntries.length === 1 ? 'y' : 'ies'}.`);
    console.error('Route new logging through LOG_E/W/I/D in src/utils/log.h (ADR-0051).');
    return 1;
  }
  return 0;
}

// Run only as a CLI, not when imported by the test.
if (import.meta.url === `file://${process.argv[1]}`) {
  process.exit(main(process.argv.slice(2)));
}
