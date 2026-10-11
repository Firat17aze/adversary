// Unit tests for the raw-Serial lint (slice-0052). Run: node --test scripts/lint-raw-serial.test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { scanText, evaluateBaseline } from './lint-raw-serial.mjs';

test('scanText finds each raw Serial.print variant', () => {
  const text = [
    'Serial.print("a");',
    'Serial.println("b");',
    'Serial.printf("c %d", 1);',
  ].join('\n');
  const findings = scanText(text);
  assert.equal(findings.length, 3);
  assert.deepEqual(findings.map(f => f.line), [1, 2, 3]);
});

test('scanText tolerates whitespace around the dot and call', () => {
  assert.equal(scanText('Serial . printf ("x");').length, 1);
});

test('scanText ignores non-print Serial methods and other objects', () => {
  const text = 'Serial.begin(115200);\nSerial.available();\nlogFile.printf("x");\nmyserial.print("y");';
  // Only `myserial.print` would NOT match (no word-boundary `Serial`); begin/available/logFile never match.
  assert.equal(scanText(text).length, 0);
});

test('evaluateBaseline flags a new raw-Serial file not in the baseline', () => {
  const has = new Map([['src/new_module.cpp', true]]);
  const { newSites, staleEntries } = evaluateBaseline(has, new Set());
  assert.deepEqual(newSites, ['src/new_module.cpp']);
  assert.deepEqual(staleEntries, []);
});

test('evaluateBaseline tolerates a baselined legacy file that still has raw Serial', () => {
  const has = new Map([['src/legacy.cpp', true]]);
  const { newSites, staleEntries } = evaluateBaseline(has, new Set(['src/legacy.cpp']));
  assert.deepEqual(newSites, []);
  assert.deepEqual(staleEntries, []);
});

test('evaluateBaseline never flags a sanctioned sink', () => {
  const has = new Map([['src/utils/log.h', true], ['src/utils/error_logger.h', true]]);
  const { newSites } = evaluateBaseline(has, new Set());
  assert.deepEqual(newSites, []);
});

test('evaluateBaseline flags a stale baseline entry once its file is clean', () => {
  // File migrated: no raw Serial left, but still listed -> must be removed (baseline only shrinks).
  const has = new Map([['src/migrated.cpp', false]]);
  const { newSites, staleEntries } = evaluateBaseline(has, new Set(['src/migrated.cpp']));
  assert.deepEqual(newSites, []);
  assert.deepEqual(staleEntries, ['src/migrated.cpp']);
});
