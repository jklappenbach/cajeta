// The old Kernels and Language models URLs redirect into the XPU tab
// (xpu-tab-and-walkthrough spec §3, plan 1.1.2). Redirects live in
// public/_redirects, Cloudflare's static redirect file, which Astro copies
// into dist/ — the directory wrangler.jsonc's assets block serves. Every
// rule's target, and every old /llm/<page>/ through the wildcard, must land
// on a page the build produced.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';
import { SITE_ROOT, docsRoot } from './docs-root.mjs';
import { urlForDoc } from './extract.mjs';

const REDIRECTS = join(SITE_ROOT, 'public', '_redirects');
const DIST = join(SITE_ROOT, 'dist');

/** Parse a Cloudflare _redirects file: `source target [status]` per line. */
export function parseRedirects(text) {
  return text
    .split(/\r?\n/)
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith('#'))
    .map((l) => {
      const [from, to, status] = l.split(/\s+/);
      return { from, to, status: status ? Number(status) : 302 };
    });
}

/** First matching rule wins; a trailing `*` captures into `:splat`. */
export function resolveRedirect(rules, path) {
  for (const r of rules) {
    if (r.from.endsWith('*')) {
      const prefix = r.from.slice(0, -1);
      if (path.startsWith(prefix)) return { ...r, location: r.to.replace(':splat', path.slice(prefix.length)) };
    } else if (r.from === path) {
      return { ...r, location: r.to };
    }
  }
  return null;
}

const rules = () => parseRedirects(readFileSync(REDIRECTS, 'utf8'));
const pageInDist = (url) => existsSync(join(DIST, url, 'index.html'));

// the old URL -> the new page, as the spec and plan list them
const EXPECTED = {
  '/kernels/': '/xpu/',
  '/kernels/llm/': '/xpu/kernels-llm/',
  '/kernels/xgboost/': '/xpu/kernels-xgboost/',
  '/kernels/catalog-llm/': '/xpu/catalog-llm/',
  '/kernels/catalog-xgboost/': '/xpu/catalog-xgboost/',
  '/llm/': '/xpu/llm/',
};

/** Every /llm/<page>/ the old tree had, read off the moved directory. */
function oldLlmPages() {
  return readdirSync(join(docsRoot(), 'xpu', 'llm'))
    .filter((f) => /\.md$/i.test(f))
    .map((f) => ({ old: urlForDoc(`llm/${f}`).replace(/^\/llm\/$/, '/llm/readme/'), now: urlForDoc(`xpu/llm/${f}`) }));
}

test('resolveRedirect: exact lines, then the wildcard with its splat', () => {
  const r = parseRedirects('# c\n/a/ /b/ 301\n/a/* /c/:splat 301\n');
  assert.equal(resolveRedirect(r, '/a/').location, '/b/');
  assert.equal(resolveRedirect(r, '/a/x/').location, '/c/x/');
  assert.equal(resolveRedirect(r, '/a/x/').status, 301);
  assert.equal(resolveRedirect(r, '/z/'), null);
});

test('_redirects sends every old Kernels and Language models URL to its XPU page with a 301', () => {
  const rs = rules();
  for (const [from, to] of Object.entries(EXPECTED)) {
    const hit = resolveRedirect(rs, from);
    assert.ok(hit, `${from} has no redirect`);
    assert.equal(hit.location, to, `${from}`);
    assert.equal(hit.status, 301, `${from} is not permanent`);
  }
  assert.ok(rs.some((r) => r.from === '/llm/*' && r.to === '/xpu/llm/:splat' && r.status === 301));
  for (const { old, now } of oldLlmPages()) {
    assert.equal(resolveRedirect(rs, old)?.location, now, `${old}`);
  }
});

test('every redirect target is a page the docs tree renders', () => {
  const urls = new Set();
  const walk = (dir, rel) => {
    for (const e of readdirSync(dir, { withFileTypes: true })) {
      if (e.name.startsWith('.')) continue;
      if (e.isDirectory()) walk(join(dir, e.name), rel + e.name + '/');
      else if (/\.md$/i.test(e.name)) urls.add(urlForDoc(rel + e.name));
    }
  };
  walk(docsRoot(), '');
  for (const r of rules()) {
    if (r.to.includes(':splat')) continue;
    assert.ok(urls.has(r.to), `${r.from} -> ${r.to}: no docs page renders there`);
  }
  for (const { now } of oldLlmPages()) assert.ok(urls.has(now), now);
});

test('after a build: _redirects ships in dist/ and every target exists there', (t) => {
  if (!existsSync(join(DIST, 'index.html'))) {
    t.skip('no dist/ — run npm run build first');
    return;
  }
  assert.equal(readFileSync(join(DIST, '_redirects'), 'utf8'), readFileSync(REDIRECTS, 'utf8'));
  const rs = rules();
  const olds = [...Object.keys(EXPECTED), ...rs.filter((r) => !r.from.includes('*')).map((r) => r.from), ...oldLlmPages().map((p) => p.old)];
  for (const old of new Set(olds)) {
    const hit = resolveRedirect(rs, old);
    assert.ok(hit, `${old} has no redirect`);
    assert.ok(pageInDist(hit.location), `${old} -> ${hit.location}: not in dist/`);
    assert.ok(!pageInDist(old), `${old} still exists in dist/ and would shadow nothing — remove the stale page`);
  }
});
