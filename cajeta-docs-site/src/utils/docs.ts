import type { CollectionEntry } from 'astro:content';

export type DocEntry = CollectionEntry<'docs'>;

// The canonical docs/ markdown has no frontmatter, so title/description/category
// are derived from the body and the file's path-slug (`entry.id`).

/** First `# Heading` in the doc, else the last path segment. */
export function docTitle(entry: DocEntry): string {
  const m = (entry.body ?? '').match(/^#\s+(.+?)\s*$/m);
  if (m) return m[1].replace(/`/g, '').trim();
  return entry.id.split('/').pop() ?? entry.id;
}

/** First real prose paragraph, markdown stripped, truncated. */
export function docDescription(entry: DocEntry, max = 200): string {
  const lines = (entry.body ?? '').split(/\r?\n/);
  const buf: string[] = [];
  let inFence = false;
  for (const raw of lines) {
    const line = raw.trim();
    if (line.startsWith('```')) {
      inFence = !inFence;
      continue;
    }
    if (inFence) continue;
    if (line === '') {
      if (buf.length) break; // end of the first paragraph
      continue;
    }
    // skip headings, blockquotes/banners, tables, raw html, and list items
    // while hunting for the first prose paragraph.
    if (/^(#|>|\||<|[-*+]\s|\d+\.\s)/.test(line)) continue;
    buf.push(line);
  }
  let text = buf
    .join(' ')
    .replace(/`([^`]*)`/g, '$1')
    .replace(/\*\*([^*]*)\*\*/g, '$1')
    .replace(/\*([^*]*)\*/g, '$1')
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1')
    .replace(/\s+/g, ' ')
    .trim();
  if (text.length > max) text = text.slice(0, max - 1).trimEnd() + '…';
  return text;
}

const CATEGORY_BY_PREFIX: Array<[string, string]> = [
  ['stdlib/', 'Stdlib'],
  ['gpu/', 'GPU'],
  ['specs/', 'Specs'],
  ['history/', 'Status'],
];

/** Coarse category from the doc's top-level directory. */
export function docCategory(entry: DocEntry): string {
  for (const [prefix, cat] of CATEGORY_BY_PREFIX) {
    if (entry.id.startsWith(prefix)) return cat;
  }
  return 'Core';
}
