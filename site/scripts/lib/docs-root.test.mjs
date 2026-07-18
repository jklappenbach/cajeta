import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { resolveDocsRoot, SITE_ROOT } from './docs-root.mjs';

// 1.1 precedence: env var > .docs-root file > ../docs default
test('env var wins over everything', () => {
  const dir = mkdtempSync(join(tmpdir(), 'droot-'));
  writeFileSync(join(dir, '.docs-root'), '/from/file');
  assert.equal(
    resolveDocsRoot({ env: '/from/env', siteRoot: dir }),
    resolve(dir, '/from/env')
  );
  rmSync(dir, { recursive: true });
});

test('.docs-root file wins over default, resolved against site root', () => {
  const dir = mkdtempSync(join(tmpdir(), 'droot-'));
  writeFileSync(join(dir, '.docs-root'), '../../cajeta-five/docs\n');
  assert.equal(
    resolveDocsRoot({ env: undefined, siteRoot: dir }),
    resolve(dir, '../../cajeta-five/docs')
  );
  rmSync(dir, { recursive: true });
});

test('defaults to ../docs beside the site', () => {
  const dir = mkdtempSync(join(tmpdir(), 'droot-'));
  assert.equal(resolveDocsRoot({ env: undefined, siteRoot: dir }), resolve(dir, '..', 'docs'));
  rmSync(dir, { recursive: true });
});

test('blank .docs-root file falls through to default', () => {
  const dir = mkdtempSync(join(tmpdir(), 'droot-'));
  writeFileSync(join(dir, '.docs-root'), '  \n');
  assert.equal(resolveDocsRoot({ env: undefined, siteRoot: dir }), resolve(dir, '..', 'docs'));
  rmSync(dir, { recursive: true });
});

test('SITE_ROOT points at the site package', () => {
  assert.ok(SITE_ROOT.endsWith('/site'));
});
