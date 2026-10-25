// benchmark-reference 3.1.2 and 3.1.3: the ```bench, ```bench-standings and
// ```bench-matrix fences become <bench-chart> placeholders carrying their
// params and a static fallback, through the site's own markdown pipeline.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createMarkdownProcessor } from '@astrojs/markdown-remark';
import rehypeBench, { syntaxHighlight } from '../../src/lib/rehype-bench.mjs';
import rehypeDocsLinks from '../../src/lib/rehype-docs-links.mjs';
import rehypeEditorial from '../../src/lib/rehype-editorial.mjs';
import { docsRoot } from './docs-root.mjs';
import { fillPlaceholders, defaultMachine, standingsModel } from './bench-render.mjs';

const BENCH = join(docsRoot(), '..', 'bench');

const entry = (over) => ({
  label: 'Cajeta', language: 'cajeta', cajeta: true, library: '', platform: 'cajeta',
  versions: { language: '0.46.3', library: '' }, backend: 'cpu', os: '', driver: '', model: '',
  min: 1000, median: 1200, p95: 1500, mean: 1250, throughput: null, unit: '',
  alloc: 800000, rss: 4096, status: 'ok', reason: '', variant: '', ...over,
});

const EMPTY = { machines: [], results: {} };
const MEASURED = {
  machines: [
    { name: 'phoenix', label: 'Phoenix', run: '20261101T0300Z-0.46.3', date: '2026-11-01' },
    { name: 'proton', label: 'Proton', run: '20261102T0300Z-0.46.3', date: '2026-11-02' },
  ],
  results: {
    proton: {
      'collection/hashmap-int': {
        '': {
          Cajeta: entry(),
          'C++': entry({ label: 'C++', language: 'cpp', cajeta: false, library: 'unordered_dense', median: 900, min: 850, p95: 990, versions: { language: 'g++ 14.2', library: '4.5.0' } }),
          Go: entry({ label: 'Go', language: 'go', cajeta: false, median: null, min: null, p95: null, alloc: null, rss: null, status: 'skipped', reason: 'toolchain not installed' }),
        },
      },
    },
    phoenix: {
      'xpu/babelstream-triad': {
        '': {
          Cajeta: entry({ backend: 'cuda', throughput: 900, unit: 'GB/s' }),
          'CUDA C++': entry({ label: 'CUDA C++', language: 'cuda-c++', cajeta: false, backend: 'cuda', throughput: 950, unit: 'GB/s' }),
        },
      },
    },
  },
};

async function render(markdown, page, data, opts = {}) {
  const processor = await createMarkdownProcessor({
    syntaxHighlight,
    rehypePlugins: [[rehypeBench, { data, ...opts }], rehypeDocsLinks, rehypeEditorial],
  });
  const { code } = await processor.render(markdown, { fileURL: pathToFileURL(join(BENCH, page)) });
  return code;
}

const HASHMAP = '# HashMap\n\n```bench\nid: collection/hashmap-int\nmetric: time\n```\n';

test('a bench block becomes a <bench-chart> island with its params', async () => {
  const html = await render(HASHMAP, 'collection/HashMap.md', EMPTY);
  assert.match(html, /<bench-chart data-kind="chart" data-id="collection\/hashmap-int" data-metric="time">/);
  assert.doesNotMatch(html, /<pre/, 'the fence is gone');
  assert.doesNotMatch(html, /id: collection/, 'the block text is gone');
});

test('an unknown id fails the build naming the page and the id', async () => {
  const md = '# X\n\n```bench\nid: collection/no-such-bench\nmetric: time\n```\n';
  await assert.rejects(
    render(md, 'collection/HashMap.md', EMPTY, { knownIds: ['collection/hashmap-int'] }),
    /bench\/collection\/HashMap\.md.*collection\/no-such-bench/
  );
});

test('a block with no id or an unknown metric fails naming the page', async () => {
  await assert.rejects(render('```bench\nmetric: time\n```\n', 'collection/HashMap.md', EMPTY), /HashMap\.md.*id/);
  await assert.rejects(
    render('```bench\nid: collection/hashmap-int\nmetric: speed\n```\n', 'collection/HashMap.md', EMPTY),
    /HashMap\.md.*metric/
  );
});

// 3.1.3 the unmeasured state
test('no row on the selected machine renders the unmeasured state and nothing else', async () => {
  const html = await render(HASHMAP, 'collection/HashMap.md', EMPTY);
  assert.match(html, /<figure class="chart bench-figure bench-unmeasured-figure"/);
  assert.match(html, /<figcaption>collection\/hashmap-int<\/figcaption>/);
  assert.match(html, /<p class="bench-unmeasured">Not yet measured on Proton\.<\/p>/);
  assert.doesNotMatch(html, /<table/);
  assert.doesNotMatch(html, /<svg/);
});

test('with rows, the fallback is a Data table, one row per language, best first', async () => {
  const html = await render(HASHMAP, 'collection/HashMap.md', MEASURED);
  assert.match(html, /<figcaption>collection\/hashmap-int · lower is better · Proton · 2026-11-02<\/figcaption>/);
  assert.match(html, /<details class="bench-data"><summary>Data<\/summary>/);
  const head = [...html.matchAll(/<th>([^<]*)<\/th>/g)].map((m) => m[1]);
  assert.deepEqual(head, ['language', 'library', 'version', 'median', 'min', 'p95', 'alloc bytes', 'peak RSS', 'status / notes']);
  const rows = [...html.matchAll(/<tr><td>([^<]*)<\/td>/g)].map((m) => m[1]);
  assert.deepEqual(rows, ['C++', 'Cajeta', 'Go'], 'sorted by median, the skipped row last');
  assert.match(html, /<td>1\.20 µs<\/td>/);
  assert.match(html, /<td>800 KB<\/td>|<td>781 KB<\/td>/);
  assert.match(html, /skipped · toolchain not installed/);
});

test('a throughput block says higher is better and sorts descending', async () => {
  const md = '```bench\nid: xpu/babelstream-triad\nmetric: throughput\n```\n';
  const html = await render(md, 'xpu/Device.md', MEASURED);
  // xpu page whose rows are cuda-only on Phoenix and absent on Proton: Phoenix is the default
  assert.match(html, /xpu\/babelstream-triad · higher is better · Phoenix · 2026-11-01/);
  const rows = [...html.matchAll(/<tr><td>([^<]*)<\/td>/g)].map((m) => m[1]);
  assert.deepEqual(rows, ['CUDA C++', 'Cajeta']);
});

test('defaultMachine: Proton, except xpu pages whose rows are cuda-only on Phoenix', () => {
  assert.equal(defaultMachine('benchmarks/collection/HashMap.md', ['collection/hashmap-int'], MEASURED), 'proton');
  assert.equal(defaultMachine('benchmarks/xpu/Device.md', ['xpu/babelstream-triad'], MEASURED), 'phoenix');
  // not an xpu page: Proton even though only Phoenix has the row
  assert.equal(defaultMachine('benchmarks/math/Tensor.md', ['xpu/babelstream-triad'], MEASURED), 'proton');
  assert.equal(defaultMachine('benchmarks/xpu/Device.md', ['xpu/babelstream-triad'], EMPTY), 'proton');
});

test('the standings block: unmeasured with no results, a table fallback with them', async () => {
  const md = '# Benchmarks\n\n## Standings\n\n```bench-standings\n```\n';
  const none = await render(md, 'README.md', EMPTY);
  assert.match(none, /<bench-chart data-kind="standings">/);
  assert.match(none, /Not yet measured on Proton\./);
  const html = await render(md, 'README.md', MEASURED);
  assert.match(html, /<th>package<\/th><th>geomean ratio<\/th><th>wins<\/th><th>losses<\/th><th>skips<\/th>/);
  // collection: Cajeta 1200 vs best competitor 900 -> 1.33x, one loss
  assert.match(html, /<tr><td>collection<\/td><td>1\.33×<\/td><td>0<\/td><td>1<\/td><td>0<\/td><\/tr>/);
});

test('standingsModel: throughput ids compare rates, packages limit and order the rows', () => {
  const idMeta = {
    'xpu/babelstream-triad': { metric: 'throughput' },
    'collection/hashmap-int': { metric: 'time' },
    'collection/tasks-footprint': { metric: 'memory' },
  };
  const rows = standingsModel(
    { ...MEASURED.results.phoenix, ...MEASURED.results.proton, 'selftest/sum': { '': { Cajeta: entry() } } },
    { idMeta, packages: ['collection', 'xpu', 'io'] }
  );
  assert.deepEqual(rows.map((r) => r.pkg), ['collection', 'xpu', 'io']);
  // triad: CUDA C++ 950 GB/s vs Cajeta 900 GB/s -> 950/900, a loss
  assert.equal(rows[1].ratio.toFixed(4), (950 / 900).toFixed(4));
  assert.equal(rows[1].losses, 1);
  assert.equal(rows[2].ratio, null);
});

test('the matrix block: params carried, unmeasured with no results, key columns in the fallback', async () => {
  const md = '```bench-matrix\nkeys: os, driver, backend\nmeasures: prefill, decode\nids: llm/prefill-512, llm/decode-1seq\n```\n';
  const none = await render(md, 'llm/README.md', EMPTY);
  assert.match(none, /<bench-chart data-kind="matrix" data-keys="os,driver,backend" data-measures="prefill,decode" data-ids="llm\/prefill-512,llm\/decode-1seq">/);
  assert.match(none, /Not yet measured on Proton\./);
  const llm = (platform, cajeta, tps, over = {}) =>
    entry({ label: platform, platform, cajeta, language: cajeta ? 'cajeta' : 'cpp', backend: 'hip', os: 'Ubuntu 26.04', driver: 'ROCm 6.4.1', model: 'Llama-3.1-8B Q4_K_M', throughput: tps, unit: 'tokens/s', ...over });
  const data = {
    machines: [{ name: 'proton', label: 'Proton', run: 'r', date: '2026-11-02' }],
    results: {
      proton: {
        'llm/prefill-512': { '': { a: llm('cajeta-llm 0.9.0 hip', true, 950), b: llm('llama.cpp b6123 hip', false, 1000) } },
        'llm/decode-1seq': { '': { a: llm('cajeta-llm 0.9.0 hip', true, 40), b: llm('llama.cpp b6123 hip', false, 50) } },
      },
    },
  };
  const html = await render(md, 'llm/README.md', data);
  assert.match(html, /Llama-3\.1-8B Q4_K_M/);
  assert.match(html, /<th>os<\/th><th>driver<\/th><th>backend<\/th>/);
  assert.match(html, /<td>Ubuntu 26\.04<\/td><td>ROCm 6\.4\.1<\/td><td>hip<\/td>/);
  assert.match(html, /950 tok\/s/);
  assert.match(html, /0\.95×/);
  assert.match(html, /0\.80×/);
});

test('fillPlaceholders re-renders a cached page against the current results', async () => {
  const cached = await render(HASHMAP, 'collection/HashMap.md', EMPTY);
  const html = fillPlaceholders(cached, { pagePath: 'benchmarks/collection/HashMap.md', data: MEASURED });
  assert.doesNotMatch(html, /Not yet measured/);
  assert.match(html, /<tr><td>C\+\+<\/td>/);
  // and the other way: a page rendered with results, filled with none
  const measured = await render(HASHMAP, 'collection/HashMap.md', MEASURED);
  assert.match(fillPlaceholders(measured, { pagePath: 'benchmarks/collection/HashMap.md', data: EMPTY }), /Not yet measured on Proton\./);
});

test('fillPlaceholders also fails on an unknown id', () => {
  const html = '<bench-chart data-kind="chart" data-id="collection/nope" data-metric="time"></bench-chart>';
  assert.throws(
    () => fillPlaceholders(html, { pagePath: 'benchmarks/collection/HashMap.md', data: { ...EMPTY, knownIds: ['collection/hashmap-int'] } }),
    /bench\/collection\/HashMap\.md.*collection\/nope/
  );
});
