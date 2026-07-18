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
} from './extract.mjs';

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

test('fileChapters does not double-number titles that already lead with the number', () => {
  const docs = [
    { slug: 'g/09-a', url: '/g/09-a/', group: '', title: '9 — Type kinds', order: 9 },
    { slug: 'g/10-b', url: '/g/10-b/', group: '', title: '10 — Allocation', order: 10 },
    { slug: 'g/11-c', url: '/g/11-c/', group: '', title: 'Ownership', order: 11 },
  ];
  const chapters = fileChapters(docs);
  assert.deepEqual(
    chapters.map((c) => c.title),
    ['9 — Type kinds', '10 — Allocation', '11. Ownership']
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
  assert.equal(
    rewriteMdHref('rust/README.md', 'benchmarks/README.md'),
    '/benchmarks/rust/readme/'
  );
  // docs files reach bench via ../../bench/… — maps into the pseudo-section
  assert.equal(
    rewriteMdHref('../../bench/README.md', 'home/README.md'),
    '/benchmarks/readme/'
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
