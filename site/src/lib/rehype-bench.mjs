// Rehype plugin: the Benchmarks pages' fenced blocks (benchmark-reference §5.2)
//
//   ```bench             id: <catalog id>, metric: time | throughput | memory
//   ```bench-standings   (no fields) the landing standings
//   ```bench-matrix      keys, measures, ids — the llm matrix
//
// become <bench-chart data-kind=… data-…> placeholders holding the static
// no-JS fallback for the page's default machine (the unmeasured state, or the
// data table). The page's React island (BenchPage.tsx) finds the placeholders
// on hydrate and renders the charts into them. A block naming an id that is
// not known (registered by the harness, named by a page, or in a result)
// fails the build with the page path and the id.
//
// Options: { data } — the bench data built by scripts/build-manifest.mjs
// (default: src/data/bench.json, or no results when it is absent);
// { knownIds } — overrides data.knownIds.
import { existsSync, readFileSync } from 'node:fs';
import { join, relative, sep } from 'node:path';
import { docsRoot, SITE_ROOT } from '../../scripts/lib/docs-root.mjs';
import {
  blockKindFor,
  parseBlock,
  blockAttrs,
  blockIds,
  checkKnownIds,
  defaultMachine,
  fallbackHtml,
} from '../../scripts/lib/bench-render.mjs';

/**
 * The markdown `syntaxHighlight` setting the plugin needs: shiki rewrites an
 * unknown fence language to plaintext, so the bench fences are excluded from
 * highlighting and reach this plugin as <pre><code class="language-bench">.
 * ('math' is shiki's own default exclusion.)
 */
export const syntaxHighlight = {
  type: 'shiki',
  excludeLangs: ['math', 'bench', 'bench-standings', 'bench-matrix'],
};

const BENCH_ROOT = join(docsRoot(), '..', 'bench') + sep;
const DATA_FILE = join(SITE_ROOT, 'src', 'data', 'bench.json');

export function loadBenchData() {
  if (!existsSync(DATA_FILE)) return { machines: [], results: {} };
  return JSON.parse(readFileSync(DATA_FILE, 'utf8'));
}

function textOf(node) {
  if (node.type === 'text') return node.value;
  return (node.children ?? []).map(textOf).join('');
}

/** The fence language of a <pre> (plain, or already highlighted by shiki). */
function fenceLang(pre) {
  if (pre.properties?.dataLanguage) return String(pre.properties.dataLanguage);
  const code = pre.children?.find((c) => c.type === 'element' && c.tagName === 'code');
  const cls = code?.properties?.className ?? [];
  const m = (Array.isArray(cls) ? cls : [cls]).map(String).map((c) => /^language-(.+)$/.exec(c)).find(Boolean);
  return m ? m[1] : null;
}

function walk(node, fn, parent = null, index = 0) {
  fn(node, parent, index);
  (node.children ?? []).forEach((child, i) => walk(child, fn, node, i));
}

export default function rehypeBench(options = {}) {
  return (tree, file) => {
    const path = file?.path ?? '';
    const pagePath = path.startsWith(BENCH_ROOT)
      ? 'benchmarks/' + relative(BENCH_ROOT, path).split(sep).join('/')
      : path
        ? relative(join(docsRoot()), path).split(sep).join('/')
        : 'unknown page';
    const found = [];
    walk(tree, (node, parent, index) => {
      if (node.type !== 'element' || node.tagName !== 'pre' || !parent) return;
      const kind = blockKindFor(fenceLang(node));
      if (!kind) return;
      let block;
      try {
        block = parseBlock(kind, textOf(node));
      } catch (err) {
        throw new Error(`${pagePath.replace(/^benchmarks\//, 'bench/')}: ${err.message}`);
      }
      found.push({ block, parent, index });
    });
    if (!found.length) return;

    const data = options.data ?? loadBenchData();
    const knownIds = options.knownIds ?? data.knownIds;
    for (const { block } of found) checkKnownIds(block, pagePath, knownIds);
    const machine = defaultMachine(pagePath, found.flatMap(({ block }) => blockIds(block)), data);
    for (const { block, parent, index } of found) {
      parent.children[index] = {
        type: 'element',
        tagName: 'bench-chart',
        properties: Object.fromEntries(
          Object.entries(blockAttrs(block)).map(([k, v]) => [
            k.replace(/^data-(.)/, (_, c) => 'data' + c.toUpperCase()),
            v,
          ])
        ),
        children: [{ type: 'raw', value: fallbackHtml(block, { machine, data }) }],
      };
    }
  };
}
