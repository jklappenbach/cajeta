// The walkthrough (plan xpu-tab-and-walkthrough 3.1.1) names kernels and
// source lines; both must stay true. Every kernel it names is a catalog row,
// every `File.cajeta:line` names a file in cajeta-llm, every relative link in
// the XPU pages resolves, and its inline SVG diagrams survive the site's
// markdown pipeline as written.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, existsSync, readdirSync, statSync } from 'node:fs';
import { join, dirname, resolve, basename, sep } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createMarkdownProcessor } from '@astrojs/markdown-remark';
import rehypeBench from '../../src/lib/rehype-bench.mjs';
import rehypeDocsLinks from '../../src/lib/rehype-docs-links.mjs';
import rehypeEditorial from '../../src/lib/rehype-editorial.mjs';
import { docsRoot } from './docs-root.mjs';
import { XPU_PAGES, linkTargets } from './xpu-docs.mjs';

const docs = docsRoot();
const bench = resolve(docs, '..', 'bench');
const walkPath = join(docs, 'xpu', 'walkthrough.md');
const walkthrough = readFileSync(walkPath, 'utf8');
const LLM_SRC = resolve(docs, '..', '..', 'cajeta-llm', 'src', 'main', 'cajeta', 'dev', 'cajeta', 'llm');

/** Backticked identifiers that name a kernel: *Kernel, Tile_*, *F32. */
export function kernelNames(md) {
  const names = [...md.matchAll(/`([A-Za-z_][A-Za-z0-9_]*)`/g)].map((m) => m[1]);
  return [...new Set(names.filter((n) => /Kernel$/.test(n) || /^Tile_/.test(n) || /F32$/.test(n)))];
}

function filesUnder(dir, out = new Set()) {
  for (const e of readdirSync(dir)) {
    const p = join(dir, e);
    if (statSync(p).isDirectory()) filesUnder(p, out);
    else out.add(e);
  }
  return out;
}

test('kernelNames picks the three shapes and nothing else', () => {
  assert.deepEqual(
    kernelNames('`fooKernel` `Tile_q8_128` `rmsNormF32` `Tensor` `@Kernel` `a.b`'),
    ['fooKernel', 'Tile_q8_128', 'rmsNormF32'],
  );
});

test('every kernel the walkthrough names is a row of catalog-llm.md', () => {
  const catalog = readFileSync(join(docs, 'xpu', 'catalog-llm.md'), 'utf8');
  const rows = new Set([...catalog.matchAll(/<code>([^<]+)<\/code>/g)].map((m) => m[1].trim()));
  const names = kernelNames(walkthrough);
  assert.ok(names.length > 0, 'the walkthrough names no kernels');
  const missing = names.filter((n) => !rows.has(n));
  assert.deepEqual(missing, [], `not in catalog-llm.md: ${missing.join(', ')}`);
});

test('every File.cajeta:line in the walkthrough names a file in cajeta-llm', (t) => {
  if (!existsSync(LLM_SRC)) return t.skip(`no cajeta-llm checkout at ${LLM_SRC}`);
  const files = filesUnder(LLM_SRC);
  const refs = [...new Set([...walkthrough.matchAll(/([A-Za-z0-9_/.-]+\.cajeta):\d+/g)].map((m) => m[1]))];
  assert.ok(refs.length > 0, 'the walkthrough cites no source lines');
  const missing = refs.filter((r) => (r.includes('/') ? !existsSync(join(LLM_SRC, r)) : !files.has(basename(r))));
  assert.deepEqual(missing, [], `not found under ${LLM_SRC}: ${missing.join(', ')}`);
});

test('every relative link in the XPU pages resolves under docs/ or bench/', () => {
  const broken = [];
  for (const page of XPU_PAGES) {
    const path = join(docs, 'xpu', page);
    const md = readFileSync(path, 'utf8');
    const targets = [...linkTargets(md), ...[...md.matchAll(/href="([^"]+)"/g)].map((m) => m[1])];
    for (const raw of targets) {
      if (/^[a-z][a-z0-9+.-]*:/i.test(raw) || raw.startsWith('#') || raw.startsWith('/')) continue;
      const target = resolve(dirname(path), decodeURI(raw.split('#')[0]));
      const inside = [docs, bench].some((root) => target === root || target.startsWith(root + sep));
      if (!inside || !existsSync(target)) broken.push(`${page}: ${raw}`);
    }
  }
  assert.deepEqual(broken, []);
});

const svgs = (html) => [...html.matchAll(/<svg[\s\S]*?<\/svg>/g)].map((m) => m[0]);
// rehype-raw re-serializes the svg: self-closing tags open and close, and
// quotes and angle brackets in text and attributes are re-encoded. Compare
// past those.
const norm = (s) =>
  s
    .replace(/<([a-zA-Z]+)((?:\s[^<>]*?)?)\s*\/>/g, '<$1$2></$1>')
    .replace(/&#x27;|&#39;/g, "'")
    .replace(/&#x3C;|&lt;/g, '<')
    .replace(/&#x3E;|&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&amp;/g, '&')
    .replace(/\s+/g, ' ')
    .replace(/\s+>/g, '>')
    .replace(/> </g, '><')
    .trim();

test("the walkthrough's diagrams survive the markdown pipeline intact", async () => {
  const processor = await createMarkdownProcessor({ rehypePlugins: [rehypeBench, rehypeDocsLinks, rehypeEditorial] });
  const { code } = await processor.render(walkthrough, { fileURL: pathToFileURL(walkPath) });
  const source = svgs(walkthrough);
  const rendered = svgs(code);
  assert.equal(source.length, 5, 'the walkthrough should carry five diagrams');
  assert.equal(rendered.length, source.length, 'a diagram was lost in rendering');
  assert.equal((code.match(/<figure class="diagram">/g) ?? []).length, 5);
  assert.equal((code.match(/<figcaption>/g) ?? []).length, 5);
  source.forEach((s, i) => assert.equal(norm(rendered[i]), norm(s), `diagram ${i + 1} changed in rendering`));
  assert.ok(!/&#x3C;svg|&lt;svg/.test(code), 'an svg was escaped as text');
});

test('marker ids are unique across the walkthrough', () => {
  const ids = [...walkthrough.matchAll(/<marker id="([^"]+)"/g)].map((m) => m[1]);
  assert.equal(new Set(ids).size, ids.length, `duplicate marker ids: ${ids.join(', ')}`);
  const refs = [...walkthrough.matchAll(/url\(#([^)]+)\)/g)].map((m) => m[1]);
  const undefinedRefs = refs.filter((r) => !ids.includes(r));
  assert.deepEqual([...new Set(undefinedRefs)], [], 'a marker reference has no definition');
});
