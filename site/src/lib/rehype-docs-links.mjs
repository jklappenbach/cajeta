// Rehype plugin: rewrite relative `*.md` hrefs inside docs (and bench)
// markdown to the site URLs the same files render at. The mapping logic
// lives in scripts/lib/extract.mjs and is unit-tested there.
import { relative, sep, join } from 'node:path';
import { rewriteMdHref } from '../../scripts/lib/extract.mjs';
import { docsRoot } from '../../scripts/lib/docs-root.mjs';

// scanned roots and the pseudo-prefix their files render under
const ROOTS = [
  { root: docsRoot() + sep, pseudo: '' },
  { root: join(docsRoot(), '..', 'bench') + sep, pseudo: 'benchmarks/' },
];

function walk(node, fn) {
  fn(node);
  for (const child of node.children ?? []) walk(child, fn);
}

export default function rehypeDocsLinks() {
  return (tree, file) => {
    if (!file.path) return;
    const owner = ROOTS.find((r) => file.path.startsWith(r.root));
    if (!owner) return;
    const fromRelPath =
      owner.pseudo + relative(owner.root, file.path).split(sep).join('/');
    walk(tree, (node) => {
      if (node.type === 'element' && node.tagName === 'a' && node.properties?.href) {
        node.properties.href = rewriteMdHref(String(node.properties.href), fromRelPath);
      }
    });
  };
}
