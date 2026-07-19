// Pure helpers shared by the manifest generator, the Astro content config,
// and the React islands. No node imports — must stay browser-safe.

/** Slug one path segment: lowercase, non-alphanumerics collapse to '-'. */
export function slugSegment(segment) {
  return segment
    .toLowerCase()
    .replace(/[^a-z0-9]+/g, '-')
    .replace(/^-+|-+$/g, '');
}

/** docs-relative path -> stable site slug ('specification/lang/memorymodel'). */
export function slugForPath(relPath) {
  return relPath
    .replace(/\.md$/i, '')
    .split('/')
    .map(slugSegment)
    .join('/');
}

/** Top-level docs/ child owning this file; loose files belong to 'guides'. */
export function sectionFor(relPath) {
  const i = relPath.indexOf('/');
  return i === -1 ? 'guides' : relPath.slice(0, i);
}

/** Site URL for a docs-relative path. */
export function urlForDoc(relPath) {
  const slug = slugForPath(relPath);
  return sectionFor(relPath) === 'guides' ? `/guides/${slug}/` : `/${slug}/`;
}

/** Strip inline markdown down to plain text. */
export function stripInline(text) {
  return text
    .replace(/\\([\\`*_{}[\]()#+\-.!<>|])/g, '$1') // markdown escapes -> literal
    .replace(/!\[([^\]]*)\]\([^)]*\)/g, '$1') // images -> alt
    .replace(/\[([^\]]*)\]\([^)]*\)/g, '$1') // links -> text
    .replace(/\[([^\]]*)\]\[[^\]]*\]/g, '$1') // ref links -> text
    .replace(/`([^`]*)`/g, '$1')
    .replace(/\*\*([^*]+)\*\*/g, '$1')
    .replace(/\*([^*]+)\*/g, '$1')
    .replace(/__([^_]+)__/g, '$1')
    .replace(/~~([^~]+)~~/g, '$1')
    .replace(/<[^>]+>/g, '')
    .replace(/\s+/g, ' ')
    .trim();
}

function* proseLines(markdown) {
  let inFence = false;
  for (const raw of markdown.split(/\r?\n/)) {
    const line = raw.trimEnd();
    if (/^\s*(```|~~~)/.test(line)) {
      inFence = !inFence;
      yield { line, fence: true, inFence: true };
      continue;
    }
    yield { line, fence: false, inFence };
  }
}

/** First `#` heading (any level, shallowest wins by order), else filename stem. */
export function titleFrom(markdown, filename) {
  let fallback = null;
  for (const { line, fence, inFence } of proseLines(markdown)) {
    if (fence || inFence) continue;
    const m = /^(#{1,6})\s+(.*)$/.exec(line);
    if (!m) continue;
    const text = stripInline(m[2]).replace(/\.md$/i, '');
    if (m[1].length === 1) return text;
    if (fallback === null) fallback = text;
  }
  return fallback ?? filename.replace(/\.md$/i, '').split('/').pop();
}

const NON_PROSE = /^(#{1,6}\s|\||>|[-*+]\s|\d+[.)]\s|<|!\[|\[!\[|={3,}|-{3,}\s*$|:{1}.*:$)/;

/** First prose paragraph, plain-texted and truncated to ~200 chars. */
export function descriptionFrom(markdown, maxLen = 200) {
  const para = [];
  for (const { line, fence, inFence } of proseLines(markdown)) {
    if (fence || inFence) {
      if (para.length) break;
      continue;
    }
    if (line.trim() === '') {
      if (para.length) break;
      continue;
    }
    if (NON_PROSE.test(line.trim())) {
      if (para.length) break;
      continue;
    }
    para.push(line.trim());
  }
  const text = stripInline(para.join(' '));
  if (text.length <= maxLen) return text;
  const cut = text.slice(0, maxLen);
  return cut.slice(0, cut.lastIndexOf(' ')) + '…';
}

/** Prose word count; fence contents excluded. */
export function wordCount(markdown) {
  let count = 0;
  for (const { line, fence, inFence } of proseLines(markdown)) {
    if (fence || inFence) continue;
    count += (line.match(/\S+/g) ?? []).length;
  }
  return count;
}

/** Tab order: guides leads, history trails, everything else alphabetical. */
export function orderTabs(sections) {
  const mid = sections.filter((s) => s !== 'guides' && s !== 'history').sort();
  return [
    ...(sections.includes('guides') ? ['guides'] : []),
    ...mid,
    ...(sections.includes('history') ? ['history'] : []),
  ];
}

/** Chapter order from a numeric filename prefix ('guide/07-comments.md' -> 7). */
export function orderFromPath(relPath) {
  const base = relPath.split('/').pop();
  const m = /^(\d+)[-_.]/.exec(base);
  return m ? parseInt(m[1], 10) : null;
}

/**
 * Chapters-as-files: from one section's manifest docs, build the ordered
 * chapter list — the section-root README as "Introduction", then the
 * numerically prefixed section-root files. Docs in subdirectories are not
 * chapters. Returns null when fewer than 3 numbered files exist.
 */
export function fileChapters(sectionDocs) {
  const rootDocs = sectionDocs.filter((d) => d.group === '');
  const numbered = rootDocs
    .filter((d) => d.order !== null && d.order !== undefined)
    .sort((a, b) => a.order - b.order);
  if (numbered.length < 3) return null;
  const readme = rootDocs.find((d) => /(^|\/)readme$/.test(d.slug));
  return [
    ...(readme ? [{ slug: readme.slug, title: 'Introduction', url: readme.url }] : []),
    ...numbered.map((d) => ({
      slug: d.slug,
      title: new RegExp(`^0*${d.order}\\b`).test(d.title) ? d.title : `${d.order}. ${d.title}`,
      url: d.url,
    })),
  ];
}

/**
 * Top tabs mirror the docs tree's top-level directories, alphabetically —
 * a new directory becomes a tab on the next regen with no code changes.
 * A loose tour doc is promoted to a leading "Tour" tab (and the loose-file
 * "Guides" pseudo-section then gets no tab); without a tour, loose files
 * keep a leading "Guides" tab.
 */
export function buildTabs(sections, docs) {
  const tourDoc = docs.find((d) => d.section === 'guides' && /^tour[-_]/.test(d.slug));
  const tabs = [];
  if (tourDoc) tabs.push({ label: 'Tour', url: tourDoc.url, section: 'tour' });
  const home = sections.find((s) => s.name === 'home');
  if (home) tabs.unshift({ label: home.label, url: '/', section: 'home' });
  const guides = sections.find((s) => s.name === 'guides');
  if (!tourDoc && guides) tabs.push({ label: guides.label, url: guides.url, section: 'guides' });
  const dirs = sections
    .filter((s) => !['guides', 'home', 'benchmarks'].includes(s.name))
    .sort((a, b) => a.name.localeCompare(b.name));
  for (const s of dirs) tabs.push({ label: s.label, url: s.url, section: s.name });
  const bench = sections.find((s) => s.name === 'benchmarks');
  if (bench) tabs.push({ label: bench.label, url: bench.url, section: 'benchmarks' });
  return tabs;
}

const byTitleNumeric = (a, b) =>
  a.order !== null && b.order !== null && a.order !== undefined && b.order !== undefined
    ? a.order - b.order
    : a.title.localeCompare(b.title, undefined, { numeric: true });

/**
 * Left-index model for a reference section: the section README becomes an
 * "Overview" entry pointing at the section root; remaining docs group by
 * subdirectory ('io/file' shown as 'io / file'), sorted numerically-aware.
 */
export function sectionNav(sectionDocs, sectionUrl) {
  const readme = sectionDocs.find((d) => d.group === '' && /(^|\/)readme$/.test(d.slug));
  const rest = sectionDocs.filter((d) => d !== readme);
  const groupNames = [...new Set(rest.map((d) => d.group))].sort();
  return {
    overview: readme ? { title: 'Overview', url: sectionUrl, slug: readme.slug } : null,
    groups: groupNames.map((name) => ({
      name: name === '' ? '' : name.split('/').join(' / '),
      docs: rest.filter((d) => d.group === name).sort(byTitleNumeric),
    })),
  };
}

/** Case-insensitive every-term match over title + description. */
export function filterMatch(doc, query) {
  const terms = query.toLowerCase().split(/\s+/).filter(Boolean);
  if (terms.length === 0) return true;
  const haystack = `${doc.title} ${doc.description}`.toLowerCase();
  return terms.every((t) => haystack.includes(t));
}

/** Resolve '.' / '..' in a slash path without node:path. */
function resolveRelative(fromDir, href) {
  const parts = fromDir ? fromDir.split('/') : [];
  for (const seg of href.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') parts.pop();
    else parts.push(seg);
  }
  return parts.join('/');
}

/**
 * Rewrite a relative `.md` href found in `fromRelPath` (docs-relative) to the
 * corresponding site URL. Anchors survive; externals, absolute paths,
 * pure anchors, and non-.md targets pass through untouched.
 */
export function rewriteMdHref(href, fromRelPath) {
  if (/^[a-z][a-z0-9+.-]*:/i.test(href)) return href; // proto: mailto: etc.
  if (href.startsWith('/') || href.startsWith('#')) return href;
  const [path, anchor] = href.split('#');
  if (!/\.md$/i.test(path)) return href;
  const fromDir = fromRelPath.includes('/')
    ? fromRelPath.slice(0, fromRelPath.lastIndexOf('/'))
    : '';
  let target = resolveRelative(fromDir, path);
  // pseudo-roots beside the docs tree (benchmarks/ = ../bench): cross-root
  // links resolve to 'docs/…' or 'bench/…' prefixes — map them back
  target = target.replace(/^docs\//, '').replace(/^bench\//, 'benchmarks/');
  return urlForDoc(target) + (anchor ? `#${anchor}` : '');
}
