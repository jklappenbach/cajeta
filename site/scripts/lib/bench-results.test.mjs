// benchmark-reference 3.1.1: bench/results/<machine>/<run>/results.csv (schema
// 1 and 2) becomes the manifest the site renders, keyed
// machine -> benchmark id -> variant -> language -> entry.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import {
  parseCsv,
  benchIdFor,
  loadResults,
  parseRegisteredList,
  knownIdSet,
  buildBenchData,
} from './bench-results.mjs';

const V1 =
  'schema_version,run_id,timestamp_unix,benchmark,area,dataset_name,dataset_bytes,dataset_sha256,input_size,language,language_version,library,library_version,flags,warmup,trials,min_ns,median_ns,mean_ns,p95_ns,throughput,throughput_unit,peak_rss_kb,cgroup_peak_kb,alloc_bytes,alloc_count,working_set_kb,status,check_ok,notes,variant,backend';
const V1_31 = V1.split(',').slice(0, 31).join(',');
const V1_30 = V1.split(',').slice(0, 30).join(',');
const V2 = V1 + ',machine,os,driver,model,platform';

function tree(files) {
  const root = mkdtempSync(join(tmpdir(), 'bench-results-'));
  for (const [path, body] of Object.entries(files)) {
    const full = join(root, path);
    mkdirSync(join(full, '..'), { recursive: true });
    writeFileSync(full, body);
  }
  return root;
}

test('parseCsv handles quoted fields with commas, quotes and newlines', () => {
  assert.deepEqual(parseCsv('a,b,c\n1,"x, y","say ""hi"""\n2,"multi\nline",\n'), [
    ['a', 'b', 'c'],
    ['1', 'x, y', 'say "hi"'],
    ['2', 'multi\nline', ''],
  ]);
  assert.deepEqual(parseCsv('a,b\r\n1,2\r\n'), [['a', 'b'], ['1', '2']]);
});

test('benchIdFor: a catalog-path area is the id; old suite areas map to their package', () => {
  // unit-2 harness: area() is the whole catalog id
  assert.equal(benchIdFor('collection/hashmap-int', 'hashmap-int'), 'collection/hashmap-int');
  // a catalog path that is a class, not the id: <area>/<benchmark>
  assert.equal(benchIdFor('collection/hashmap', 'insert'), 'collection/hashmap/insert');
  // with the known ids, the id that exists wins
  const known = new Set(['xpu/babelstream-triad', 'collection/hashmap-int-insert', 'math/matmul', 'lang/dot-product']);
  assert.equal(benchIdFor('xpu/babelstream', 'triad', known), 'xpu/babelstream-triad');
  assert.equal(benchIdFor('collection/hashmap', 'hashmap-int-insert', known), 'collection/hashmap-int-insert');
  // old areas
  assert.equal(benchIdFor('hash', 'xxhash3'), 'hash/xxhash3');
  assert.equal(benchIdFor('string', 'string-search'), 'lang/string-search');
  assert.equal(benchIdFor('stream', 'stream-filter-map-reduce'), 'lang/stream-filter-map-reduce');
  assert.equal(benchIdFor('clbg', 'clbg-mandelbrot'), 'lang/clbg-mandelbrot');
  assert.equal(benchIdFor('sort', 'sort-int64'), 'collection/sort-int64');
  assert.equal(benchIdFor('gpu', 'saxpy'), 'xpu/saxpy');
  assert.equal(benchIdFor('math', 'dot-product', known), 'lang/dot-product');
  // math/matmul stayed in math/: the known id wins over the lang/ default
  assert.equal(benchIdFor('math', 'matmul', known), 'math/matmul');
  assert.equal(benchIdFor('math', 'matmul'), 'lang/matmul');
});

test('loadResults: schema 2 rows keyed machine -> id -> variant -> language', () => {
  const root = tree({
    'phoenix/LATEST': '20261101T0300Z-0.46.3\n',
    'phoenix/20261101T0300Z-0.46.3/results.csv': [
      V2,
      '2,r,0,hashmap-int,collection/hashmap-int,,-1,,-1,cajeta,0.46.3,,,--release,20,200,1000,1200,1250,1500,,,4096,-1,800000,12,0,ok,true,,,cpu,phoenix,Ubuntu 24.04 (WSL2),,,cajeta',
      '2,r,0,hashmap-int,collection/hashmap-int,,-1,,-1,cpp,g++ 14.2,unordered_dense,4.5.0,-O3,20,200,900,1100,1150,1300,,,5000,-1,900000,10,0,ok,true,,,cpu,phoenix,Ubuntu 24.04 (WSL2),,,unordered_dense',
      '2,r,0,hashmap-int,collection/hashmap-int,,-1,,-1,cpp,g++ 14.2,std::unordered_map,,-O3,20,200,2000,2100,2150,2300,,,5000,-1,900000,10,0,ok,true,,,cpu,phoenix,Ubuntu 24.04 (WSL2),,,std',
      '2,r,0,hashmap-int,collection/hashmap-int,,-1,,-1,go,1.24,,,,20,200,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,false,toolchain not installed,,cpu,phoenix,Ubuntu 24.04 (WSL2),,,go',
      '2,r,0,triad,xpu/babelstream,,-1,,-1,cuda-c++,12.6,,,,20,200,10,12,12,15,900,GB/s,-1,-1,-1,-1,-1,failed,false,kaboom,,cuda,phoenix,Ubuntu 24.04 (WSL2),NVIDIA 610.88 CUDA 13.3,,BabelStream 5.0 cuda',
      '2,r,0,hashmap-int,collection/hashmap-int,,-1,,-1,rust,1.88,,,,1,3,10,12,12,15,,,1,-1,0,-1,0,ok,true,smoke,,cpu,phoenix,Ubuntu 24.04 (WSL2),,,rust',
    ].join('\n') + '\n',
    'phoenix/20261101T0300Z-0.46.3/env.csv': 'cpu_model,AMD Ryzen 9 3950X\ncajeta_version,0.46.3\n',
    // an older run LATEST does not name is never read
    'phoenix/20261001T0300Z-0.46.0/results.csv':
      V2 + '\n2,r,0,old,collection/old,,-1,,-1,cajeta,0.46.0,,,,20,200,1,1,1,1,,,1,-1,0,-1,0,ok,true,,,cpu,phoenix,,,,cajeta\n',
  });
  try {
    const data = loadResults(root);
    const phoenix = data.results.phoenix;
    assert.ok(phoenix, 'phoenix machine present');
    assert.equal(data.machines.find((m) => m.name === 'phoenix').run, '20261101T0300Z-0.46.3');
    assert.equal(data.machines.find((m) => m.name === 'phoenix').date, '2026-11-01');
    assert.deepEqual(data.machines.find((m) => m.name === 'phoenix').runs, ['20261001T0300Z-0.46.0', '20261101T0300Z-0.46.3']);
    assert.equal(data.machines.find((m) => m.name === 'phoenix').env.cpu_model, 'AMD Ryzen 9 3950X');
    assert.equal(phoenix['collection/old'], undefined, 'LATEST selects the run');

    const hm = phoenix['collection/hashmap-int'][''];
    // two C++ rows are told apart by library; the smoke Rust row is dropped
    assert.deepEqual(Object.keys(hm).sort(), ['C++ · std::unordered_map', 'C++ · unordered_dense', 'Cajeta', 'Go']);
    const c = hm.Cajeta;
    assert.equal(c.min, 1000);
    assert.equal(c.median, 1200);
    assert.equal(c.p95, 1500);
    assert.equal(c.alloc, 800000);
    assert.equal(c.rss, 4096);
    assert.equal(c.status, 'ok');
    assert.equal(c.cajeta, true);
    assert.deepEqual(c.versions, { language: '0.46.3', library: '' });
    assert.deepEqual(hm['C++ · unordered_dense'].versions, { language: 'g++ 14.2', library: '4.5.0' });
    assert.equal(hm['C++ · unordered_dense'].library, 'unordered_dense');
    assert.equal(hm.Go.status, 'skipped');
    assert.equal(hm.Go.reason, 'toolchain not installed');
    assert.equal(hm.Go.median, null, '-1 is no value');

    const triad = phoenix['xpu/babelstream/triad'][''];
    assert.equal(triad['CUDA C++'].status, 'failed');
    assert.equal(triad['CUDA C++'].reason, 'kaboom');
    assert.equal(triad['CUDA C++'].backend, 'cuda');
    assert.equal(triad['CUDA C++'].driver, 'NVIDIA 610.88 CUDA 13.3');
    assert.equal(triad['CUDA C++'].throughput, 900);
    assert.equal(triad['CUDA C++'].unit, 'GB/s');
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('loadResults: schema 1 files (30, 31 and 32 columns) load, variants keyed', () => {
  const root = tree({
    // no LATEST: the newest run is the default
    'proton/20260625-170906/results.csv': [
      V1_31,
      '1,20260625-170906,,variant-demo,selftest,,-1,,-1,cajeta,,,,--release,3,20,10,20,20,40,,,3076,-1,0,-1,0,ok,true,,small',
      '1,20260625-170906,,variant-demo,selftest,,-1,,-1,cajeta,,,,--release,3,20,10,20,19,21,,,3080,-1,0,-1,0,ok,true,,large',
      '1,20260625-170906,,xxhash3,hash,,-1,,-1,rust,1.88,xxhash-rust,0.8,,20,200,50,60,60,70,,,1,-1,10,-1,0,ok,true,,',
    ].join('\n') + '\n',
    'proton/20260101-000000/results.csv':
      V1_30 + '\n1,x,,string-search,string,,-1,,-1,cajeta,,,,,1,1,1,1,1,1,,,1,-1,0,-1,0,ok,true,\n',
    'proton/20260102-000000/results.csv':
      V1 + '\n1,x,,matmul,math,,-1,,-1,cajeta,,,,,20,200,50000,60000,60000,70000,,,1,-1,10,-1,0,ok,true,,,cpu\n',
  });
  try {
    const data = loadResults(root);
    const m = data.machines.find((x) => x.name === 'proton');
    assert.equal(m.run, '20260625-170906');
    assert.equal(m.date, '2026-06-25');
    const proton = data.results.proton;
    assert.deepEqual(Object.keys(proton['selftest/variant-demo']).sort(), ['large', 'small']);
    assert.equal(proton['selftest/variant-demo'].small.Cajeta.median, 20);
    assert.equal(proton['hash/xxhash3'][''].Rust.versions.library, '0.8');
    assert.equal(proton['hash/xxhash3'][''].Rust.machine, 'proton', 'schema 1 rows take the directory machine');

    // the 30- and 32-column shapes parse too
    const thirty = loadResults(root, { run: { proton: '20260101-000000' } });
    assert.equal(thirty.results.proton['lang/string-search'][''].Cajeta.median, 1);
    const thirtyTwo = loadResults(root, { run: { proton: '20260102-000000' } });
    assert.equal(thirtyTwo.results.proton['lang/matmul'][''].Cajeta.backend, 'cpu');
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('loadResults: no results directory yields an empty manifest, not an error', () => {
  const data = loadResults(join(tmpdir(), 'no-such-bench-results-dir'));
  assert.deepEqual(data.machines, []);
  assert.deepEqual(data.results, {});
});

test('loadResults: an unknown header shape fails naming the file', () => {
  const root = tree({ 'phoenix/r1/results.csv': 'a,b,c\n1,2,3\n' });
  try {
    assert.throws(() => loadResults(root), /phoenix\/r1\/results\.csv.*header/);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('loadResults: identical rows are numbered, not padded with fields that tell nothing apart', () => {
  const row = '2,r,0,hashmap-int,collection/hashmap-int,,-1,,-1,cajeta,0.46.3,,,,20,200,1,2,2,3,,,1,-1,0,-1,0,ok,true,,,cpu,phoenix,Ubuntu 24.04,,,cajeta';
  const root = tree({ 'phoenix/r/results.csv': [V2, row, row].join('\n') + '\n' });
  try {
    assert.deepEqual(Object.keys(loadResults(root).results.phoenix['collection/hashmap-int']['']), ['Cajeta', 'Cajeta (2)']);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

test('parseRegisteredList reads `bench.sh --list` lines into catalog ids', () => {
  const text = 'collection/hashmap-int/hashmap-int  [cpu]\nxpu/gemm-f32/gemm-f32  [cuda]\n\n# comment\nhash/xxhash3/xxhash3\n';
  assert.deepEqual(parseRegisteredList(text), ['collection/hashmap-int', 'xpu/gemm-f32', 'hash/xxhash3']);
});

test('parseRegisteredList keeps a catalog id as listed: no old-area mapping (math/ stays math/)', () => {
  // dev.cajeta.bench 0.1.5 lists `<package>/<slug>`; `math` and `string` are
  // also old flat area names, which must not be remapped to lang/ here.
  const text = '  math/matmul  [cpu]\n  lang/saxpy  [cpu]\n  math/fft-1d  [cpu]\n  collection/sort-int64  [cpu]\n';
  assert.deepEqual(parseRegisteredList(text), ['math/matmul', 'lang/saxpy', 'math/fft-1d', 'collection/sort-int64']);
});

test('knownIdSet: page ids plus result ids, or the registered list when there is one', () => {
  const pageIds = ['collection/hashmap-int', 'llm/decode-1seq', 'collection/typo'];
  const results = { phoenix: { 'hash/xxhash3': {} } };
  const loose = knownIdSet({ pageIds, results });
  assert.ok(loose.has('collection/typo') && loose.has('hash/xxhash3'));
  // with a registered list, a page id in a package the list covers must be registered;
  // packages the list does not cover (the ecosystem libraries) keep their page ids
  const strict = knownIdSet({ pageIds, results, registered: ['collection/hashmap-int'] });
  assert.ok(strict.has('collection/hashmap-int'));
  assert.ok(!strict.has('collection/typo'));
  assert.ok(strict.has('llm/decode-1seq'));
  assert.ok(strict.has('hash/xxhash3'));
});

test('knownIdSet: ecosystem ids on a covered package\'s page are known (cajeta-codec\'s deflate rows)', () => {
  const pageIds = ['codec/base64-encode', 'codec/deflate-oneshot', 'codec/typo'];
  const known = knownIdSet({ pageIds, registered: ['codec/base64-encode'], ecosystem: ['codec/deflate-oneshot'] });
  assert.ok(known.has('codec/base64-encode'));
  assert.ok(known.has('codec/deflate-oneshot'));
  assert.ok(!known.has('codec/typo'));
});

test('buildBenchData reads bench/registered-ids.txt and bench/ecosystem-ids.txt', () => {
  const root = tree({
    'codec/Deflate.md': '# Deflate\n\n```bench\nid: codec/deflate-oneshot\n```\n',
    'codec/Base64.md': '# Base64\n\n```bench\nid: codec/base64-encode\n```\n',
    'math/Fft.md': '# Fft\n\n```bench\nid: math/fft-1d\n```\n',
    'registered-ids.txt': '# header\n  codec/base64-encode  [cpu]\n  math/fft-1d  [cpu]\n',
    'ecosystem-ids.txt': '# header\ncodec/deflate-oneshot  cajeta-codec\n',
  });
  const data = buildBenchData(root, { env: {} });
  assert.ok(data.registered);
  for (const id of ['codec/base64-encode', 'codec/deflate-oneshot', 'math/fft-1d']) assert.ok(data.knownIds.includes(id), id);
  assert.ok(!data.knownIds.includes('lang/fft-1d'));
});
