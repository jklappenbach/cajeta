import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';
import { existsSync } from 'node:fs';
import { join } from 'node:path';
// @ts-ignore -- plain mjs helpers shared with the manifest generator
import { slugForPath } from '../scripts/lib/extract.mjs';
// @ts-ignore
import { docsRoot } from '../scripts/lib/docs-root.mjs';

// The docs tree is read *in place* (see scripts/lib/docs-root.mjs for how
// the root is chosen). Ids match the manifest slugs exactly because both
// derive from the same slugForPath().
const docs = defineCollection({
  loader: glob({
    pattern: '**/*.md',
    base: docsRoot(),
    generateId: ({ entry }) => slugForPath(entry),
  }),
});

// The benchmarks live beside the docs tree in bench/; their ids get the
// 'benchmarks/' pseudo-section prefix to match the manifest.
const benchRoot = join(docsRoot(), '..', 'bench');
const bench = defineCollection({
  loader: glob({
    pattern: existsSync(benchRoot) ? '**/*.md' : '!**',
    base: existsSync(benchRoot) ? benchRoot : docsRoot(),
    generateId: ({ entry }) => 'benchmarks/' + slugForPath(entry),
  }),
});

export const collections = { docs, bench };
