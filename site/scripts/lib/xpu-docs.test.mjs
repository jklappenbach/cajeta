// The XPU tab's pages (plan xpu-tab-and-walkthrough 2.1.1): the map, the
// walkthrough, the scheduler and optimization pages and the three child
// landings exist, each opens with a title and a prose paragraph, each landing
// points at its reference pages, and the map's stack table links every layer.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync } from 'node:fs';
import { join } from 'node:path';
import { docsRoot } from './docs-root.mjs';

import { XPU_PAGES, linkTargets } from './xpu-docs.mjs';

const xpu = (p) => join(docsRoot(), 'xpu', p);
const read = (p) => readFileSync(xpu(p), 'utf8');

function opening(md) {
  const lines = md.split('\n');
  const h1 = lines.findIndex((l) => /^# \S/.test(l));
  let para = '';
  for (let i = h1 + 1; i < lines.length; i++) {
    const l = lines[i];
    if (!l.trim()) { if (para) break; continue; }
    if (!para && /^(#|<|\||[-*+] |\d+\. |```|>)/.test(l.trim())) break;
    para += (para ? ' ' : '') + l.trim();
  }
  return { h1: h1 >= 0 ? lines[h1].slice(2).trim() : null, para };
}

for (const page of XPU_PAGES) {
  test(`${page} exists with an H1 title and a first prose paragraph`, () => {
    assert.ok(existsSync(xpu(page)), `${page} is missing`);
    const { h1, para } = opening(read(page));
    assert.ok(h1, `${page} has no H1`);
    assert.ok(para.length >= 60, `${page}: the H1 is not followed by a prose paragraph (got "${para}")`);
  });
}

test('the math landing links its stdlib pages', () => {
  const links = linkTargets(read('math/README.md')).filter((l) => l.includes('/stdlib/math/'));
  for (const name of ['Tensor', 'DType', 'linalg/LinAlg', 'fft/Fft']) {
    assert.ok(links.some((l) => l.endsWith(`/stdlib/math/${name}.md`)), `no link to stdlib/math/${name}.md`);
  }
});

test('the nucleo landing links its stdlib pages', () => {
  const links = linkTargets(read('nucleo/README.md')).filter((l) => l.includes('/stdlib/nucleo/'));
  for (const name of ['Column', 'Table', 'Tape', 'Fuse']) {
    assert.ok(links.some((l) => l.endsWith(`/stdlib/nucleo/${name}.md`)), `no link to stdlib/nucleo/${name}.md`);
  }
});

test("the ml landing links the library repository's docs and the xgboost catalog", () => {
  const links = linkTargets(read('ml/README.md'));
  assert.ok(
    links.some((l) => /^https:\/\/github\.com\/[^/]+\/cajeta-ml\/blob\/[^/]+\/docs\//.test(l)),
    'no link into the cajeta-ml repository docs',
  );
  assert.ok(links.some((l) => l.endsWith('catalog-xgboost.md')), 'no link to the xgboost catalog');
});

test('the map has a stack table linking every layer and the walkthrough', () => {
  const md = read('README.md');
  const section = md.split(/^## The stack\s*$/m)[1]?.split(/^## /m)[0];
  assert.ok(section, 'README.md has no "## The stack" section');
  const table = section.split('\n').filter((l) => l.startsWith('|')).join('\n');
  assert.ok(table, 'the stack section has no table');
  const links = linkTargets(table);
  for (const t of ['scheduler.md', 'optimization.md', 'math/README.md', 'nucleo/README.md', 'ml/README.md', 'llm/README.md']) {
    assert.ok(links.includes(t), `the stack table does not link ${t}`);
  }
  assert.ok(linkTargets(section).includes('walkthrough.md'), 'the stack section does not link walkthrough.md');
});
