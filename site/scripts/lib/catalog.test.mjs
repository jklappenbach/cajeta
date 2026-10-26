// The kernel catalog pages (docs/xpu/catalog-*.md) render as tables that
// cannot wrap at the prose width: a few narrow grid columns, and each kernel's
// role on its own full-width row underneath (spec benchmark-reference 10.3).
// The pages are rendered here through the site's own markdown pipeline.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createMarkdownProcessor } from '@astrojs/markdown-remark';
import rehypeDocsLinks from '../../src/lib/rehype-docs-links.mjs';
import rehypeEditorial from '../../src/lib/rehype-editorial.mjs';
import { docsRoot } from './docs-root.mjs';

async function render(markdown, path) {
  const processor = await createMarkdownProcessor({
    rehypePlugins: [rehypeDocsLinks, rehypeEditorial],
  });
  const { code } = await processor.render(markdown, path ? { fileURL: pathToFileURL(path) } : {});
  return code;
}

async function renderPage(name) {
  const path = join(docsRoot(), 'xpu', name);
  return render(readFileSync(path, 'utf8'), path);
}

const strip = (s) => s.replace(/<[^>]+>/g, '').trim();

function headers(html) {
  return [...html.matchAll(/<thead>([\s\S]*?)<\/thead>/g)].map((m) =>
    [...m[1].matchAll(/<th[^>]*>([\s\S]*?)<\/th>/g)].map((h) => strip(h[1])),
  );
}


function assertPaired(html) {
  const body = [...html.matchAll(/<tbody>([\s\S]*?)<\/tbody>/g)].map((m) => m[1]).join('');
  const rows = [...body.matchAll(/<tr(?: class="([^"]*)")?/g)].map((m) => m[1] ?? '');
  assert.ok(rows.length > 0, 'no body rows');
  rows.forEach((cls, i) => {
    if (i % 2 === 0) assert.equal(cls, 'kernel', `row ${i} should be a kernel row`);
    else assert.equal(cls, 'kernel-role', `row ${i} (after ${rows[i - 1]}) should be its role row`);
  });
  return rows.length / 2;
}

function assertWrapped(html) {
  const tables = (html.match(/<table/g) ?? []).length;
  const wrapped = (html.match(/<div class="table-scroll">\s*<table/g) ?? []).length;
  assert.ok(tables > 0, 'no table rendered');
  assert.equal(wrapped, tables, 'every table sits in a .table-scroll container');
}

test('a raw HTML table in markdown is kept and wrapped in .table-scroll', async () => {
  const html = await render('Some prose that is long enough to be the lede of the page, surely.\n\n<table>\n<tr><td>x</td></tr>\n</table>\n');
  assert.match(html, /<div class="table-scroll">\s*<table>/);
});

test('a markdown table is still wrapped in .table-scroll', async () => {
  const html = await render('| a | b |\n|---|---|\n| 1 | 2 |\n');
  assert.match(html, /<div class="table-scroll">\s*<table>/);
});

test('catalog-llm: at most five header cells, kernel / counterpart / cuda / cpu / hip', async () => {
  const html = await renderPage('catalog-llm.md');
  assertWrapped(html);
  const heads = headers(html);
  for (const h of heads) assert.ok(h.length <= 5, `header ${JSON.stringify(h)} has more than five cells`);
  assert.deepEqual(heads[0], ['kernel', 'llama.cpp counterpart', 'cuda', 'cpu', 'hip']);
});

test('catalog-llm: every kernel row is followed by its role row', async () => {
  const html = await renderPage('catalog-llm.md');
  assert.ok(assertPaired(html) >= 258);
  assert.match(html, /<tr class="kernel-role"><td colspan="5">/);
});

test('catalog-xgboost: kernel / stage / @Wave(32), every kernel row followed by its role row', async () => {
  const html = await renderPage('catalog-xgboost.md');
  assertWrapped(html);
  assert.deepEqual(headers(html), [['kernel', 'stage', '@Wave(32)']]);
  assert.ok(assertPaired(html) >= 33);
  assert.match(html, /<tr class="kernel-role"><td colspan="3">/);
});
