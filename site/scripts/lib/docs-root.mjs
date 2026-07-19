// Where the docs tree lives. Resolution order:
//   1. CAJETA_DOCS_ROOT env var
//   2. a gitignored `site/.docs-root` file containing a path
//   3. ../docs (this repository's tree)
// Paths are resolved against the site package root, so relative overrides
// like `../../cajeta-five/docs` work regardless of cwd.
import { readFileSync, existsSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

export const SITE_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');

export function resolveDocsRoot({ env, siteRoot } = {}) {
  const root = siteRoot ?? SITE_ROOT;
  const fromEnv = env ?? process.env.CAJETA_DOCS_ROOT;
  if (fromEnv && fromEnv.trim()) return resolve(root, fromEnv.trim());
  const marker = resolve(root, '.docs-root');
  if (existsSync(marker)) {
    const p = readFileSync(marker, 'utf8').trim();
    if (p) return resolve(root, p);
  }
  return resolve(root, '..', 'docs');
}

/** The active docs root for this build. */
export function docsRoot() {
  return resolveDocsRoot({});
}
