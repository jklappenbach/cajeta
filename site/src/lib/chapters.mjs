// Chapter splitting for numbered guides. Pure string/HTML logic so it is
// unit-testable under node --test (scripts/lib/chapters.test.mjs).
//
// A doc qualifies when it has >= MIN_CHAPTERS h2 headings whose text starts
// with "N." / "N)". Its rendered HTML is split into an intro chunk (before
// chapter 1) plus one chunk per numbered h2; unnumbered h2s stay inside the
// running chapter. Same-page anchors are rewritten to the owning chapter.

const MIN_CHAPTERS = 3;
const NUMBERED = /^\d+[.)]\s/;

/**
 * @param {string} html      rendered HTML of the whole doc
 * @param {{depth:number, slug:string, text:string}[]} headings
 * @param {string} baseUrl   guide base URL, with trailing slash
 * @returns {null | {slug:string, title:string, url:string, html:string}[]}
 */
export function splitChapters(html, headings, baseUrl) {
  const chapterHeads = headings.filter((h) => h.depth === 2 && NUMBERED.test(h.text));
  if (chapterHeads.length < MIN_CHAPTERS) return null;

  // cut points: the <h2 id="<slug>"> of each numbered chapter, in order
  const cuts = [];
  for (const h of chapterHeads) {
    const idx = html.indexOf(`<h2 id="${h.slug}"`);
    if (idx === -1) return null; // ids out of sync with html — do not chapterize
    cuts.push({ ...h, idx });
  }
  cuts.sort((a, b) => a.idx - b.idx);

  const chapters = [];
  chapters.push({
    slug: '',
    title: 'Introduction',
    url: baseUrl,
    html: html.slice(0, cuts[0].idx),
  });
  cuts.forEach((cut, i) => {
    const end = i + 1 < cuts.length ? cuts[i + 1].idx : html.length;
    chapters.push({
      slug: cut.slug,
      title: cut.text,
      url: `${baseUrl}${cut.slug}/`,
      html: html.slice(cut.idx, end),
    });
  });

  // map every element id to the chapter chunk that contains it
  const idOwner = new Map();
  for (const ch of chapters) {
    for (const m of ch.html.matchAll(/\bid="([^"]+)"/g)) idOwner.set(m[1], ch);
  }

  // rewrite same-page anchors that point into a *different* chapter
  for (const ch of chapters) {
    ch.html = ch.html.replace(/href="#([^"]+)"/g, (full, id) => {
      const owner = idOwner.get(id);
      if (!owner || owner === ch) return full;
      return `href="${owner.url}#${id}"`;
    });
  }

  return chapters;
}
