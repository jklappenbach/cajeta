#!/usr/bin/env node
// Scans the canonical ../docs tree and emits src/data/manifest.json — the
// single structural input for navigation tabs, section indexes, cards,
// charts, and the client-side filter. Rerun via `npm run manifest` (also
// runs automatically before `npm run dev` / `npm run build`).

import { readdir, readFile, mkdir, writeFile } from 'node:fs/promises';
import { join, relative, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  slugForPath,
  sectionFor,
  urlForDoc,
  titleFrom,
  descriptionFrom,
  wordCount,
  orderTabs,
  orderFromPath,
  buildTabs,
} from './lib/extract.mjs';
import { docsRoot, SITE_ROOT } from './lib/docs-root.mjs';

const DOCS_ROOT = docsRoot();
const OUT_FILE = join(SITE_ROOT, 'src', 'data', 'manifest.json');

// scanned roots: the docs tree, plus its sibling bench/ mapped to a
// "benchmarks" pseudo-section when present
import { existsSync } from 'node:fs';
const BENCH_ROOT = join(DOCS_ROOT, '..', 'bench');
const ROOTS = [
  { root: DOCS_ROOT, pseudo: '', srcPrefix: 'docs/' },
  ...(existsSync(BENCH_ROOT) ? [{ root: BENCH_ROOT, pseudo: 'benchmarks/', srcPrefix: 'bench/' }] : []),
];

const SECTION_LABELS = {
  guides: 'Guides',
  guide: 'Guide',
  home: 'Home',
  benchmarks: 'Benchmarks',
  stdlib: 'Stdlib',
  buildtool: 'Build Tool',
  cajeta: 'Cajeta',
  gpu: 'GPU',
  specification: 'Specification',
  specs: 'Work Specs',
  history: 'History',
};

function labelFor(section) {
  return (
    SECTION_LABELS[section] ??
    section.replace(/[-_]+/g, ' ').replace(/\b\w/g, (c) => c.toUpperCase())
  );
}

async function* mdFiles(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    if (entry.name.startsWith('.')) continue;
    const full = join(dir, entry.name);
    if (entry.isDirectory()) yield* mdFiles(full);
    else if (/\.md$/i.test(entry.name)) yield full;
  }
}

const docs = [];
const slugs = new Map();
for (const { root, pseudo, srcPrefix } of ROOTS) {
  for await (const file of mdFiles(root)) {
    const realRel = relative(root, file).split('\\').join('/');
    const relPath = pseudo + realRel; // path inside the *site's* tree model
    const body = await readFile(file, 'utf8');
    const slug = slugForPath(relPath);
    if (slugs.has(slug)) {
      throw new Error(`slug collision: ${relPath} vs ${slugs.get(slug)} -> ${slug}`);
    }
    slugs.set(slug, relPath);
    const words = wordCount(body);
    docs.push({
      path: relPath,
      src: srcPrefix + realRel, // true repository location, for display
      slug,
      url: urlForDoc(relPath),
      section: sectionFor(relPath),
      // subdirectory *within* the section, for sub-grouping ('' = section root)
      group: relPath.split('/').slice(1, -1).join('/'),
      title: titleFrom(body, relPath),
      description: descriptionFrom(body),
      order: orderFromPath(relPath),
      words,
      minutes: Math.max(1, Math.round(words / 220)),
    });
  }
}

docs.sort((a, b) => a.path.localeCompare(b.path));

const sectionNames = orderTabs([...new Set(docs.map((d) => d.section))]);
const sections = sectionNames.map((name) => {
  const inSection = docs.filter((d) => d.section === name);
  return {
    name,
    label: labelFor(name),
    url: `/${name}/`,
    count: inSection.length,
    words: inSection.reduce((n, d) => n + d.words, 0),
    groups: [...new Set(inSection.map((d) => d.group))].sort(),
  };
});

const manifest = {
  generatedFrom: relative(SITE_ROOT, DOCS_ROOT),
  sections,
  tabs: buildTabs(sections, docs),
  docs,
  stats: {
    totalDocs: docs.length,
    totalWords: docs.reduce((n, d) => n + d.words, 0),
  },
};

await mkdir(dirname(OUT_FILE), { recursive: true });
await writeFile(OUT_FILE, JSON.stringify(manifest, null, 2) + '\n');
console.log(
  `manifest: ${docs.length} docs, ${sections.length} sections -> ${relative(process.cwd(), OUT_FILE)}`
);
