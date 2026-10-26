import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  slugForPath,
  titleFrom,
  descriptionFrom,
  wordCount,
  sectionFor,
  urlForDoc,
  orderTabs,
  filterMatch,
  rewriteMdHref,
  orderFromPath,
  fileChapters,
  buildTabs,
  sectionNav,
  routeSlugFor,
  benchPageFor,
  isBenchCatalogPath,
  BENCH_CATALOG_GLOB,
  labelFor,
  REF_SECTIONS,
} from './extract.mjs';
import { docsRoot } from './docs-root.mjs';
import { readdirSync } from 'node:fs';
import { glob } from 'tinyglobby';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

// 2.1.4 slug derivation
test('slugForPath lowercases and strips .md', () => {
  assert.equal(slugForPath('LanguageGuide.md'), 'languageguide');
  assert.equal(slugForPath('specification/lang/MemoryModel.md'), 'specification/lang/memorymodel');
});

test('slugForPath collapses non-alphanumerics to single dashes', () => {
  assert.equal(slugForPath('DI-override-hook.md'), 'di-override-hook');
  assert.equal(slugForPath('specs/gpu_f16 recipe.md'), 'specs/gpu-f16-recipe');
  assert.equal(slugForPath('a/--Weird__Name--.md'), 'a/weird-name');
});

// 2.1.1 title extraction
test('titleFrom takes the first # heading', () => {
  assert.equal(titleFrom('# The Cajeta Language\n\nBody.', 'X.md'), 'The Cajeta Language');
});

test('titleFrom strips inline markdown from the heading', () => {
  assert.equal(titleFrom('# `cajeta` **Build** Tool\n', 'X.md'), 'cajeta Build Tool');
});

test('titleFrom falls back to any heading level, then filename', () => {
  assert.equal(titleFrom('## Deep Heading\ntext', 'X.md'), 'Deep Heading');
  assert.equal(titleFrom('no headings here', 'CaptureConversion.md'), 'CaptureConversion');
});

test('titleFrom unescapes markdown escapes and drops generic params', () => {
  assert.equal(titleFrom('# ArrayList\\<T\\>\n\nBody.', 'ArrayList.md'), 'ArrayList');
  assert.equal(titleFrom('# HashMap\\<K, V\\>\n', 'HashMap.md'), 'HashMap');
});

test('titleFrom drops a filename-style .md suffix in headings', () => {
  assert.equal(titleFrom('# BuildTool.md\n\nText.', 'BuildTool.md'), 'BuildTool');
});

test('titleFrom ignores headings inside code fences', () => {
  const md = '```sh\n# not a title\n```\n\n# Real Title\n';
  assert.equal(titleFrom(md, 'X.md'), 'Real Title');
});

// 2.1.2 description extraction
test('descriptionFrom takes the first prose paragraph', () => {
  const md = '# Title\n\n## Sub\n\nThis is the intro paragraph.\nIt continues here.\n\nSecond para.';
  assert.equal(descriptionFrom(md), 'This is the intro paragraph. It continues here.');
});

test('descriptionFrom skips fences, tables, lists, quotes, html', () => {
  const md = [
    '# T',
    '',
    '```cajeta',
    'class A {}',
    '```',
    '',
    '| a | b |',
    '| - | - |',
    '',
    '> quoted',
    '',
    '- item',
    '',
    '<div>html</div>',
    '',
    'The actual prose paragraph.',
  ].join('\n');
  assert.equal(descriptionFrom(md), 'The actual prose paragraph.');
});

test('descriptionFrom strips inline markdown and truncates long text', () => {
  const long = 'A **bold** [link](x.md) and `code`. ' + 'word '.repeat(100);
  const d = descriptionFrom('# T\n\n' + long);
  assert.ok(d.startsWith('A bold link and code.'));
  assert.ok(d.length <= 220);
});

test('descriptionFrom returns empty string when no prose exists', () => {
  assert.equal(descriptionFrom('# T\n\n```c\nx\n```\n'), '');
});

// 2.1.5 word count
test('wordCount counts prose words, not fence contents', () => {
  assert.equal(wordCount('one two three\n\n```\nskip me\n```\nfour'), 4);
});

// 2.1.3 section derivation
test('sectionFor maps top-level dir, loose files to guides', () => {
  assert.equal(sectionFor('specification/lang/MemoryModel.md'), 'specification');
  assert.equal(sectionFor('gpu/Matmul.md'), 'gpu');
  assert.equal(sectionFor('LanguageGuide.md'), 'guides');
});

test('urlForDoc prefixes guides for loose files, keeps dir paths', () => {
  assert.equal(urlForDoc('LanguageGuide.md'), '/guides/languageguide/');
  assert.equal(urlForDoc('specification/lang/MemoryModel.md'), '/specification/lang/memorymodel/');
});

// 4.1.1 tab ordering: guides first, history last, dirs alphabetical between
test('orderTabs puts guides first and history last', () => {
  const tabs = orderTabs(['specs', 'history', 'cajeta', 'guides', 'gpu', 'buildtool', 'specification']);
  assert.deepEqual(tabs, ['guides', 'buildtool', 'cajeta', 'gpu', 'specification', 'specs', 'history']);
});

// 5.1.1 filter matching
test('filterMatch is case-insensitive and multi-term over title+description', () => {
  const doc = { title: 'Memory Model', description: 'Borrow-checked ownership semantics.' };
  assert.ok(filterMatch(doc, 'memory'));
  assert.ok(filterMatch(doc, 'BORROW model'));
  assert.ok(!filterMatch(doc, 'gpu memory'));
  assert.ok(filterMatch(doc, '  '));
});

// docs-root-switch 2.1: numeric filename prefixes -> chapter order
test('orderFromPath reads numeric filename prefixes', () => {
  assert.equal(orderFromPath('guide/00-introduction.md'), 0);
  assert.equal(orderFromPath('guide/21-reflection.md'), 21);
  assert.equal(orderFromPath('guide/README.md'), null);
  assert.equal(orderFromPath('specification/lang/MemoryModel.md'), null);
});

// docs-root-switch 2.1: chapter list from a section's docs
test('fileChapters builds README intro + numerically ordered chapters', () => {
  const docs = [
    { path: 'guide/10-allocation.md', slug: 'guide/10-allocation', url: '/guide/10-allocation/', section: 'guide', group: '', title: 'Allocation', order: 10 },
    { path: 'guide/02-kick.md', slug: 'guide/02-kick', url: '/guide/02-kick/', section: 'guide', group: '', title: 'Kick the tires', order: 2 },
    { path: 'guide/README.md', slug: 'guide/readme', url: '/guide/readme/', section: 'guide', group: '', title: 'The Cajeta Guide', order: null },
    { path: 'guide/01-install.md', slug: 'guide/01-install', url: '/guide/01-install/', section: 'guide', group: '', title: 'Installation', order: 1 },
    { path: 'guide/drafts/x.md', slug: 'guide/drafts/x', url: '/guide/drafts/x/', section: 'guide', group: 'drafts', title: 'Draft', order: null },
  ];
  const chapters = fileChapters(docs);
  assert.deepEqual(
    chapters.map((c) => c.title),
    ['Introduction', '1. Installation', '2. Kick the tires', '10. Allocation']
  );
  assert.equal(chapters[0].slug, 'guide/readme');
  // drafts subdirectory doc is not a chapter
  assert.ok(!chapters.some((c) => c.slug === 'guide/drafts/x'));
});

test('fileChapters labels chapters "N. Name", normalizing a dash separator', () => {
  const docs = [
    { slug: 'g/09-a', url: '/g/09-a/', group: '', title: '9 — Type kinds', order: 9 },
    { slug: 'g/10-b', url: '/g/10-b/', group: '', title: '10 — Allocation', order: 10 },
    { slug: 'g/11-c', url: '/g/11-c/', group: '', title: 'Ownership', order: 11 },
    { slug: 'g/12-d', url: '/g/12-d/', group: '', title: '12. Statements', order: 12 },
    { slug: 'g/13-e', url: '/g/13-e/', group: '', title: '13 - Expressions', order: 13 },
  ];
  const chapters = fileChapters(docs);
  assert.deepEqual(
    chapters.map((c) => c.title),
    ['9. Type kinds', '10. Allocation', '11. Ownership', '12. Statements', '13. Expressions']
  );
});

test('fileChapters returns null under 3 numbered files or without README intro anchor', () => {
  const two = [
    { slug: 'a/01-x', url: '/a/01-x/', group: '', title: 'X', order: 1 },
    { slug: 'a/02-y', url: '/a/02-y/', group: '', title: 'Y', order: 2 },
  ];
  assert.equal(fileChapters(two), null);
});

// four-tabs 1.1: tabs mirror the docs tree's top-level directories
test('buildTabs is the directory list, with the loose tour promoted first', () => {
  const sections = [
    { name: 'guides', label: 'Guides', url: '/guides/' },
    { name: 'stdlib', label: 'Stdlib', url: '/stdlib/' },
    { name: 'guide', label: 'Guide', url: '/guide/' },
    { name: 'specification', label: 'Specification', url: '/specification/' },
  ];
  const docs = [
    { slug: 'tour-build-your-first-package', section: 'guides', url: '/guides/tour-build-your-first-package/' },
  ];
  assert.deepEqual(
    buildTabs(sections, docs).map((t) => `${t.label}:${t.url}`),
    [
      'Tour:/guides/tour-build-your-first-package/',
      'Guide:/guide/',
      'Specification:/specification/',
      'Stdlib:/stdlib/',
    ]
  );
});

// home-and-benchmarks 1.1: pinned tabs
test('buildTabs pins home first at / and benchmarks last', () => {
  const sections = [
    { name: 'stdlib', label: 'Stdlib', url: '/stdlib/' },
    { name: 'benchmarks', label: 'Benchmarks', url: '/benchmarks/' },
    { name: 'home', label: 'Home', url: '/home/' },
    { name: 'guide', label: 'Guide', url: '/guide/' },
    { name: 'specification', label: 'Specification', url: '/specification/' },
  ];
  assert.deepEqual(
    buildTabs(sections, []).map((t) => `${t.label}:${t.url}`),
    ['Home:/', 'Guide:/guide/', 'Specification:/specification/', 'Stdlib:/stdlib/', 'Benchmarks:/benchmarks/']
  );
});

test('rewriteMdHref maps ../docs/ links from bench pseudo-paths into the docs tree', () => {
  assert.equal(
    rewriteMdHref('../docs/specification/codec/json/Json.md', 'benchmarks/README.md'),
    '/specification/codec/json/json/'
  );
  // a package README is the package index (benchmark-reference 3.1.4)
  assert.equal(
    rewriteMdHref('rust/README.md', 'benchmarks/README.md'),
    '/benchmarks/rust/'
  );
  // docs files reach bench via ../../bench/… — maps into the pseudo-section;
  // the bench README is the Benchmarks landing
  assert.equal(
    rewriteMdHref('../../bench/README.md', 'home/README.md'),
    '/benchmarks/'
  );
});

test('buildTabs adds new directories automatically, alphabetically', () => {
  const sections = [
    { name: 'stdlib', label: 'Stdlib', url: '/stdlib/' },
    { name: 'guide', label: 'Guide', url: '/guide/' },
    { name: 'cookbook', label: 'Cookbook', url: '/cookbook/' },
  ];
  assert.deepEqual(
    buildTabs(sections, [{ slug: 'tour-x', section: 'guides', url: '/guides/tour-x/' }]).map((t) => t.label),
    ['Tour', 'Cookbook', 'Guide', 'Stdlib']
  );
});

test('buildTabs without a tour keeps a leading Guides tab for loose files', () => {
  const sections = [
    { name: 'history', label: 'History', url: '/history/' },
    { name: 'guides', label: 'Guides', url: '/guides/' },
    { name: 'gpu', label: 'GPU', url: '/gpu/' },
    { name: 'specification', label: 'Specification', url: '/specification/' },
  ];
  assert.deepEqual(
    buildTabs(sections, []).map((t) => t.label),
    ['Guides', 'GPU', 'History', 'Specification']
  );
});

// four-tabs 1.1: section nav grouping
test('sectionNav pulls README out as Overview and groups the rest', () => {
  const docs = [
    { slug: 'stdlib/readme', url: '/stdlib/readme/', group: '', title: 'Cajeta stdlib', order: null },
    { slug: 'stdlib/io/file/file', url: '/stdlib/io/file/file/', group: 'io/file', title: 'File', order: null },
    { slug: 'stdlib/io/file/path', url: '/stdlib/io/file/path/', group: 'io/file', title: 'Path', order: null },
    { slug: 'stdlib/error/throwable', url: '/stdlib/error/throwable/', group: 'error', title: 'Throwable', order: null },
    { slug: 'stdlib/lang/string10', url: '/stdlib/lang/string10/', group: 'lang', title: 'String10', order: null },
    { slug: 'stdlib/lang/string2', url: '/stdlib/lang/string2/', group: 'lang', title: 'String2', order: null },
  ];
  const nav = sectionNav(docs, '/stdlib/');
  assert.equal(nav.overview.title, 'Overview');
  assert.equal(nav.overview.url, '/stdlib/');
  assert.deepEqual(
    nav.groups.map((g) => g.name),
    ['error', 'io / file', 'lang']
  );
  // numeric-aware sort within groups
  assert.deepEqual(nav.groups[2].docs.map((d) => d.title), ['String2', 'String10']);
});

test('sectionNav without a README yields null overview', () => {
  const nav = sectionNav(
    [{ slug: 'specification/lang/x', url: '/specification/lang/x/', group: 'lang', title: 'X', order: null }],
    '/specification/'
  );
  assert.equal(nav.overview, null);
});

// 3.1.1 relative .md link rewriting
test('rewriteMdHref resolves relative .md links to site urls', () => {
  assert.equal(rewriteMdHref('BuildTool.md', 'LanguageGuide.md'), '/guides/buildtool/');
  assert.equal(rewriteMdHref('lang/MemoryModel.md', 'specification/Overview.md'), '/specification/lang/memorymodel/');
  assert.equal(rewriteMdHref('../../gpu/Matmul.md', 'specification/lang/X.md'), '/gpu/matmul/');
  assert.equal(rewriteMdHref('../concurrent/Atomics.md', 'specification/lang/X.md'), '/specification/concurrent/atomics/');
});

test('rewriteMdHref keeps anchors and leaves non-md links alone', () => {
  assert.equal(rewriteMdHref('Compilation.md#phases', 'LanguageGuide.md'), '/guides/compilation/#phases');
  assert.equal(rewriteMdHref('#local', 'a/b.md'), '#local');
  assert.equal(rewriteMdHref('https://example.com/x.md', 'a/b.md'), 'https://example.com/x.md');
  assert.equal(rewriteMdHref('mailto:x@y.z', 'a/b.md'), 'mailto:x@y.z');
  assert.equal(rewriteMdHref('image.png', 'a/b.md'), 'image.png');
});

// benchmark-reference 3.1.4: bench/<pkg>/README.md is the package index,
// bench/README.md the Benchmarks landing; tab order unchanged
test('urlForDoc: a bench package README is the package index, the bench README the landing', () => {
  assert.equal(urlForDoc('benchmarks/README.md'), '/benchmarks/');
  assert.equal(urlForDoc('benchmarks/collection/README.md'), '/benchmarks/collection/');
  assert.equal(urlForDoc('benchmarks/codec/json/README.md'), '/benchmarks/codec/json/');
  assert.equal(urlForDoc('benchmarks/collection/HashMap.md'), '/benchmarks/collection/hashmap/');
  // other sections keep their README pages where they were
  assert.equal(urlForDoc('specification/lang/README.md'), '/specification/lang/readme/');
});

test('routeSlugFor: package READMEs route at their directory, the section README at none', () => {
  assert.equal(routeSlugFor('benchmarks/collection/readme'), 'benchmarks/collection');
  assert.equal(routeSlugFor('benchmarks/readme'), null);
  assert.equal(routeSlugFor('benchmarks/collection/hashmap'), 'benchmarks/collection/hashmap');
  assert.equal(routeSlugFor('specification/lang/readme'), 'specification/lang/readme');
  assert.equal(routeSlugFor('languageguide'), 'guides/languageguide');
});

test('rewriteMdHref: bench package links reach the index, class pages reach the stdlib page', () => {
  assert.equal(rewriteMdHref('collection/README.md', 'benchmarks/README.md'), '/benchmarks/collection/');
  assert.equal(rewriteMdHref('README.md', 'benchmarks/collection/HashMap.md'), '/benchmarks/collection/');
  assert.equal(rewriteMdHref('../README.md', 'benchmarks/collection/HashMap.md'), '/benchmarks/');
  assert.equal(
    rewriteMdHref('../../docs/stdlib/collection/HashMap.md', 'benchmarks/collection/HashMap.md'),
    '/stdlib/collection/hashmap/'
  );
  assert.equal(
    rewriteMdHref('../../../docs/stdlib/collection/ltm/LtmBPlusTree.md', 'benchmarks/collection/ltm/LtmBPlusTree.md'),
    '/stdlib/collection/ltm/ltmbplustree/'
  );
});

test('benchPageFor finds the bench page of a stdlib class page, case-insensitively', () => {
  const docs = [
    { path: 'benchmarks/collection/HashMap.md', slug: 'benchmarks/collection/hashmap', url: '/benchmarks/collection/hashmap/' },
    { path: 'benchmarks/codec/json/Json.md', slug: 'benchmarks/codec/json/json', url: '/benchmarks/codec/json/json/' },
    { path: 'stdlib/collection/HashMap.md', slug: 'stdlib/collection/hashmap', url: '/stdlib/collection/hashmap/' },
  ];
  assert.equal(benchPageFor('stdlib/collection/HashMap.md', docs)?.url, '/benchmarks/collection/hashmap/');
  assert.equal(benchPageFor('stdlib/codec/json/Json.md', docs)?.url, '/benchmarks/codec/json/json/');
  assert.equal(benchPageFor('stdlib/collection/Heap.md', docs), null);
  assert.equal(benchPageFor('specification/lang/X.md', docs), null);
  assert.equal(benchPageFor('stdlib/README.md', docs), null);
});

test('sectionNav lists a package README first in its group as Overview', () => {
  const docs = [
    { slug: 'benchmarks/readme', url: '/benchmarks/', group: '', title: 'Benchmarks', order: null },
    { slug: 'benchmarks/collection/hashmap', url: '/benchmarks/collection/hashmap/', group: 'collection', title: 'HashMap', order: null },
    { slug: 'benchmarks/collection/readme', url: '/benchmarks/collection/', group: 'collection', title: 'cajeta.collection', order: null },
    { slug: 'benchmarks/collection/arraylist', url: '/benchmarks/collection/arraylist/', group: 'collection', title: 'ArrayList', order: null },
  ];
  const nav = sectionNav(docs, '/benchmarks/');
  assert.equal(nav.overview.url, '/benchmarks/');
  assert.deepEqual(nav.groups[0].docs.map((d) => d.title), ['Overview', 'ArrayList', 'HashMap']);
  assert.equal(nav.groups[0].docs[0].url, '/benchmarks/collection/');
});

test('buildTabs keeps Benchmarks pinned last with package index docs present', () => {
  const sections = [
    { name: 'benchmarks', label: 'Benchmarks', url: '/benchmarks/' },
    { name: 'stdlib', label: 'Stdlib', url: '/stdlib/' },
    { name: 'guide', label: 'Guide', url: '/guide/' },
  ];
  const docs = [{ slug: 'benchmarks/collection/readme', section: 'benchmarks', url: '/benchmarks/collection/' }];
  assert.deepEqual(buildTabs(sections, docs).map((t) => t.label), ['Guide', 'Stdlib', 'Benchmarks']);
});

// only catalog pages are documents: bench/*.md and bench/<package>/**/*.md
const BENCH_TREE = {
  'README.md': true,
  'METHODOLOGY.md': true,
  'collection/README.md': true,
  'collection/HashMap.md': true,
  'collection/ltm/LtmBPlusTree.md': true,
  'competitors/rust/codec/bind/README.md': false,
  'competitors/third_party/capnproto/doc/_posts/2013-04-01-announcing.md': false,
  'src/main/cajeta/bench/NOTES.md': false,
  'datasets/README.md': false,
  'test/README.md': false,
  'scripts/README.md': false,
  'results/phoenix/r1/NOTES.md': false,
  'build/bench/x.md': false,
  'collection/HashMap.cajeta': false,
};

test('isBenchCatalogPath keeps catalog pages and drops sources, checkouts and runs', () => {
  for (const [path, keep] of Object.entries(BENCH_TREE)) {
    assert.equal(isBenchCatalogPath(path), keep, path);
  }
});

test('BENCH_CATALOG_GLOB (the content collection) loads exactly the catalog pages', async () => {
  const root = mkdtempSync(join(tmpdir(), 'bench-glob-'));
  try {
    for (const path of Object.keys(BENCH_TREE)) {
      mkdirSync(join(root, path, '..'), { recursive: true });
      writeFileSync(join(root, path), '# x\n');
    }
    const found = (await glob(BENCH_CATALOG_GLOB, { cwd: root })).sort();
    const want = Object.entries(BENCH_TREE).filter(([, k]) => k).map(([p]) => p).sort();
    assert.deepEqual(found, want);
  } finally {
    rmSync(root, { recursive: true, force: true });
  }
});

// xpu-tab-and-walkthrough 1.1.1: one XPU tab, its sidebar in stack order
test('labelFor names the XPU tab and keeps the others', () => {
  assert.equal(labelFor('xpu'), 'XPU');
  assert.equal(labelFor('benchmarks'), 'Benchmarks');
  assert.equal(labelFor('specification'), 'Specification');
  assert.equal(labelFor('some_new-dir'), 'Some New Dir');
});

test('buildTabs: Home, Guide, Specification, Stdlib, XPU, Benchmarks', () => {
  const sections = ['benchmarks', 'xpu', 'stdlib', 'home', 'guide', 'specification'].map((name) => ({
    name,
    label: labelFor(name),
    url: `/${name}/`,
  }));
  assert.deepEqual(
    buildTabs(sections, []).map((t) => `${t.label}:${t.url}`),
    ['Home:/', 'Guide:/guide/', 'Specification:/specification/', 'Stdlib:/stdlib/', 'XPU:/xpu/', 'Benchmarks:/benchmarks/']
  );
});

test('the docs tree itself yields the tabs Home, Guide, Specification, Stdlib, XPU, Benchmarks', () => {
  const dirs = readdirSync(docsRoot(), { withFileTypes: true })
    .filter((e) => e.isDirectory() && !e.name.startsWith('.'))
    .map((e) => e.name);
  const sections = [...dirs, 'benchmarks'].map((name) => ({ name, label: labelFor(name), url: `/${name}/` }));
  assert.deepEqual(
    buildTabs(sections, []).map((t) => t.label),
    ['Home', 'Guide', 'Specification', 'Stdlib', 'XPU', 'Benchmarks']
  );
});

test('xpu is a reference section with a sidebar, its READMEs index their directories', () => {
  assert.ok(REF_SECTIONS.includes('xpu'));
  assert.equal(urlForDoc('xpu/README.md'), '/xpu/');
  assert.equal(urlForDoc('xpu/llm/README.md'), '/xpu/llm/');
  assert.equal(urlForDoc('xpu/llm/measuring.md'), '/xpu/llm/measuring/');
  assert.equal(urlForDoc('xpu/kernels-llm.md'), '/xpu/kernels-llm/');
  assert.equal(routeSlugFor('xpu/readme'), null);
  assert.equal(routeSlugFor('xpu/llm/readme'), 'xpu/llm');
  assert.equal(rewriteMdHref('../xpu/catalog-llm.md', 'guide/25-xpu-kernels.md'), '/xpu/catalog-llm/');
  assert.equal(rewriteMdHref('../../docs/xpu/llm/measuring.md', 'benchmarks/llm/Decode.md'), '/xpu/llm/measuring/');
});

const xpuDoc = (rel, title) => {
  const path = `xpu/${rel}`;
  return {
    slug: slugForPath(path),
    url: urlForDoc(path),
    group: path.split('/').slice(1, -1).join('/'),
    title,
    order: null,
  };
};

test('sectionNav(xpu): own pages in stack order, then Math, Nucleo, ML, LLM', () => {
  // shuffled, and titles chosen so an alphabetical sort would differ
  const docs = [
    xpuDoc('llm/serving.md', 'Serving'),
    xpuDoc('catalog-xgboost.md', 'Catalog: every cajeta-xgboost kernel'),
    xpuDoc('ml/README.md', 'Machine learning'),
    xpuDoc('optimization.md', 'Optimization'),
    xpuDoc('llm/README.md', 'Language models'),
    xpuDoc('kernels-xgboost.md', 'Gradient boosting on the device'),
    xpuDoc('README.md', 'One kernel, any device'),
    xpuDoc('math/README.md', 'Math'),
    xpuDoc('scheduler.md', 'The tile scheduler'),
    xpuDoc('catalog-llm.md', 'Catalog: every cajeta-llm kernel'),
    xpuDoc('llm/engine.md', 'Engine'),
    xpuDoc('walkthrough.md', 'Walkthrough'),
    xpuDoc('nucleo/README.md', 'Nucleo'),
    xpuDoc('kernels-llm.md', 'Language models on the device'),
  ];
  const nav = sectionNav(docs, '/xpu/');
  assert.equal(nav.overview.title, 'Overview');
  assert.equal(nav.overview.url, '/xpu/');
  assert.deepEqual(nav.groups.map((g) => g.name), ['', 'Math', 'Nucleo', 'ML', 'LLM']);
  assert.deepEqual(
    nav.groups[0].docs.map((d) => d.slug),
    [
      'xpu/walkthrough',
      'xpu/scheduler',
      'xpu/optimization',
      'xpu/kernels-llm',
      'xpu/kernels-xgboost',
      'xpu/catalog-llm',
      'xpu/catalog-xgboost',
    ]
  );
  assert.deepEqual(nav.groups[4].docs.map((d) => d.title), ['Overview', 'Engine', 'Serving']);
  assert.equal(nav.groups[4].docs[0].url, '/xpu/llm/');
});

test('sectionNav(xpu): pages not yet written are simply absent, extras trail', () => {
  const docs = [
    xpuDoc('catalog-llm.md', 'Catalog: every cajeta-llm kernel'),
    xpuDoc('llm/README.md', 'Language models'),
    xpuDoc('README.md', 'One kernel, any device'),
    xpuDoc('kernels-llm.md', 'Language models on the device'),
    xpuDoc('aaa-extra.md', 'Aaa extra'),
  ];
  const nav = sectionNav(docs, '/xpu/');
  assert.deepEqual(nav.groups.map((g) => g.name), ['', 'LLM']);
  assert.deepEqual(nav.groups[0].docs.map((d) => d.slug), ['xpu/kernels-llm', 'xpu/catalog-llm', 'xpu/aaa-extra']);
});

test('sectionNav elsewhere stays alphabetical', () => {
  const docs = [
    { slug: 'stdlib/zeta/z', url: '/stdlib/zeta/z/', group: 'zeta', title: 'Z', order: null },
    { slug: 'stdlib/llm/b', url: '/stdlib/llm/b/', group: 'llm', title: 'B', order: null },
    { slug: 'stdlib/math/a', url: '/stdlib/math/a/', group: 'math', title: 'A', order: null },
  ];
  assert.deepEqual(sectionNav(docs, '/stdlib/').groups.map((g) => g.name), ['llm', 'math', 'zeta']);
});
