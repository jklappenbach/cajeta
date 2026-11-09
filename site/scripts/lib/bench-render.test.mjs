import test from 'node:test';
import assert from 'node:assert/strict';
import { standingsModel } from './bench-render.mjs';

const entry = (cajeta, median, throughput, unit, status = 'ok') => ({ cajeta, median, throughput, unit, status });
const idMeta = { 'codec/base32-decode': { metric: 'throughput' }, 'io/server-echo': { metric: 'throughput' }, 'lang/loop': { metric: 'time' } };

test('a throughput benchmark whose sides name different units is ranked by median time', () => {
  // Cajeta wrote bytes/s under an empty unit; the competitor wrote MB/s.
  const res = {
    'codec/base32-decode': {
      '': { a: entry(true, 1_004_000, 1_044_437_936, ''), b: entry(false, 351_000, 2846.6, 'MB/s') },
    },
  };
  const [row] = standingsModel(res, { idMeta, packages: ['codec'] });
  assert.equal(row.n, 1);
  assert.equal(row.losses, 1);
  assert.equal(row.wins, 0);
  assert.ok(Math.abs(row.ratio - 1_004_000 / 351_000) < 1e-9, `ratio ${row.ratio}`);
});

test('a throughput benchmark whose sides share a unit is ranked by throughput', () => {
  // Time-bounded runs: equal medians, the rate is the measure.
  const res = {
    'io/server-echo': {
      '': { a: entry(true, 10_000_000_000, 5000, 'messages/s'), b: entry(false, 10_000_000_000, 10000, 'messages/s') },
    },
  };
  const [row] = standingsModel(res, { idMeta, packages: ['io'] });
  assert.equal(row.losses, 1);
  assert.ok(Math.abs(row.ratio - 2) < 1e-9, `ratio ${row.ratio}`);
});

test('a time benchmark ignores throughput even when both sides carry one', () => {
  const res = { 'lang/loop': { '': { a: entry(true, 200, 5, 'Mop/s'), b: entry(false, 100, 999, 'Mop/s') } } };
  const [row] = standingsModel(res, { idMeta, packages: ['lang'] });
  assert.ok(Math.abs(row.ratio - 2) < 1e-9, `ratio ${row.ratio}`);
});
