#include "cajetadoc/Render.h"

namespace cajetadoc {

// Themeable, React-adoptable stylesheet (plan §11):
//  - All rules scoped under `.cajetadoc` via zero-specificity :where().
//  - Cascade layers let a host site slot its own layer above ours.
//  - Every token reads var(--cajetadoc-x, var(--host, <default>)) so when
//    embedded in a parent (e.g. cajeta.dev Tailwind v4 @theme) the host tokens
//    win automatically; standalone, the built-in defaults apply.
//  - Logical properties + a dark-scheme block via prefers-color-scheme.
std::string defaultStylesheet() {
    return R"CSS(@layer cajetadoc.reset, cajetadoc.base, cajetadoc.components, cajetadoc.theme;

@layer cajetadoc.theme {
  .cajetadoc {
    --cajetadoc-bg:        var(--background, #fffaf3);
    --cajetadoc-fg:        var(--foreground, #2b2118);
    --cajetadoc-muted:     var(--muted-foreground, #7c6f63);
    --cajetadoc-accent:    var(--primary, #b5651d);
    --cajetadoc-border:    var(--border, #e7dccb);
    --cajetadoc-code-bg:   var(--muted, #f3ece0);
    --cajetadoc-badge-bg:  var(--secondary, #efe2cf);
    --cajetadoc-radius:    var(--radius, 0.5rem);
    --cajetadoc-font:      var(--font-sans, ui-sans-serif, system-ui, sans-serif);
    --cajetadoc-mono:      var(--font-mono, ui-monospace, SFMono-Regular, monospace);
    /* Layout rails: the left nav card width, the gap to the content, and the
       derived inset that the right margin mirrors. Body text/tables/cards are
       inset on the right by --cajetadoc-inset; heading rules span past it. */
    --cajetadoc-rail:      15rem;
    --cajetadoc-rail-gap:  2.5rem;
    --cajetadoc-inset:     calc(var(--cajetadoc-rail) + var(--cajetadoc-rail-gap));
    /* Approx fixed-header height; the side nav sticks just below it. */
    --cajetadoc-header-h:  5.25rem;
  }
  @media (prefers-color-scheme: dark) {
    .cajetadoc {
      --cajetadoc-bg:      var(--background, #1c1611);
      --cajetadoc-fg:      var(--foreground, #ece0d0);
      --cajetadoc-muted:   var(--muted-foreground, #b7a892);
      --cajetadoc-accent:  var(--primary, #e0945a);
      --cajetadoc-border:  var(--border, #3a2f24);
      --cajetadoc-code-bg: var(--muted, #2a2017);
      --cajetadoc-badge-bg:var(--secondary, #33271b);
    }
  }
}

@layer cajetadoc.base {
  /* Paint the whole page canvas (html/body) so short pages show no white gap
     below the content, and there are no stark white side gutters. --cajetadoc-bg
     is only defined ON .cajetadoc (custom properties inherit downward, not up),
     so the canvas reads the host token / built-in default directly. :where()
     keeps zero specificity so a host site still wins. */
  :where(html):has(.cajetadoc), :where(body):has(.cajetadoc) {
    background: var(--background, #fffaf3);
  }
  :where(body):has(.cajetadoc) { margin: 0; }
  @media (prefers-color-scheme: dark) {
    :where(html):has(.cajetadoc), :where(body):has(.cajetadoc) {
      background: var(--background, #1c1611);
    }
  }
  :where(.cajetadoc) {
    color: var(--cajetadoc-fg);
    background: var(--cajetadoc-bg);
    font-family: var(--cajetadoc-font);
    line-height: 1.6;
    inline-size: 100%;
    min-block-size: 100vh; /* fill the viewport so short pages have no gap */
    box-sizing: border-box;
    container-type: inline-size;
  }
  /* Page body below the fixed header carries the responsive side gutters and the
     vertical rhythm; the header spans edge-to-edge and pins to the top. */
  :where(.cajetadoc .doc-body) {
    padding-inline: clamp(1rem, 3vw, 2.75rem);
    padding-block: 1.5rem 3rem;
  }
  :where(.cajetadoc *) { box-sizing: border-box; }
  :where(.cajetadoc a) { color: var(--cajetadoc-accent); text-decoration: none; }
  :where(.cajetadoc a:hover) { text-decoration: underline; }
  :where(.cajetadoc code) {
    font-family: var(--cajetadoc-mono);
    background: var(--cajetadoc-code-bg);
    padding-inline: 0.3em; padding-block: 0.1em;
    border-radius: 0.3em;
    font-size: 0.92em;
  }
  :where(.cajetadoc pre) {
    background: var(--cajetadoc-code-bg);
    padding: 1rem;
    border-radius: var(--cajetadoc-radius);
    overflow-x: auto;
  }
  :where(.cajetadoc pre code) { background: none; padding: 0; }
  :where(.cajetadoc table) { border-collapse: collapse; inline-size: 100%; margin-block: 1rem; }
  :where(.cajetadoc th, .cajetadoc td) {
    border: 1px solid var(--cajetadoc-border);
    padding: 0.4rem 0.6rem;
    text-align: start;
    vertical-align: top;
  }
  :where(.cajetadoc h1, .cajetadoc h2, .cajetadoc h3, .cajetadoc h4) {
    line-height: 1.25; margin-block: 1.4em 0.5em;
  }
  :where(.cajetadoc blockquote) {
    border-inline-start: 3px solid var(--cajetadoc-accent);
    margin-inline: 0; padding-inline-start: 1rem; color: var(--cajetadoc-muted);
  }
}

@layer cajetadoc.components {
  :where(.cajetadoc .pkg-label) { color: var(--cajetadoc-muted); font-family: var(--cajetadoc-mono); }
  :where(.cajetadoc .kind) {
    color: var(--cajetadoc-accent); font-weight: 600; text-transform: lowercase;
  }
  :where(.cajetadoc .type-header) { border-block-end: 2px solid var(--cajetadoc-border); padding-block-end: 0.6rem; }
  :where(.cajetadoc .rel) { color: var(--cajetadoc-muted); font-size: 0.95em; }
  :where(.cajetadoc .member) {
    border: 1px solid var(--cajetadoc-border);
    border-radius: var(--cajetadoc-radius);
    padding: 0.75rem 1rem; margin-block: 1rem;
  }
  :where(.cajetadoc .member.deprecated) { opacity: 0.75; }
  :where(.cajetadoc .member-sig code) { background: none; font-size: 1rem; font-weight: 600; }
  :where(.cajetadoc .deprecation) {
    border-inline-start: 3px solid #c0392b; padding-inline-start: 0.75rem; margin-block: 0.5rem;
  }
  :where(.cajetadoc .tag-block h4) { margin-block: 0.6em 0.2em; color: var(--cajetadoc-muted); }
  :where(.cajetadoc .params .pname) { white-space: nowrap; }
  :where(.cajetadoc .badges) { margin-inline-start: 0.5rem; }
  :where(.cajetadoc .badge) {
    display: inline-block;
    font-family: var(--cajetadoc-mono);
    font-size: 0.72rem;
    background: var(--cajetadoc-badge-bg);
    color: var(--cajetadoc-fg);
    border-radius: 999px;
    padding-inline: 0.55em; padding-block: 0.12em;
    margin-inline-end: 0.25rem;
    vertical-align: middle;
  }
  :where(.cajetadoc .badge-fiber-unsafe, .cajetadoc .badge-blocks) { color: #c0392b; }
  :where(.cajetadoc .summary .msig) { white-space: nowrap; }
  :where(.cajetadoc .cajetadoc-footer) {
    margin-block-start: 2rem; padding-block-start: 1rem;
    border-block-start: 1px solid var(--cajetadoc-border);
    color: var(--cajetadoc-muted); font-size: 0.85em;
  }
  :where(.cajetadoc .muted) { color: var(--cajetadoc-muted); }

  /* Fixed top header bar: always present, pinned to the viewport top, does not
     scroll with the page. Two rows — brand/meta, then breadcrumbs + nav. */
  :where(.cajetadoc .topbar) {
    position: sticky; inset-block-start: 0; z-index: 20;
    background: var(--cajetadoc-bg);
    border-block-end: 1px solid var(--cajetadoc-border);
    padding-inline: clamp(1rem, 3vw, 2.75rem);
    padding-block: 0.55rem 0.5rem;
  }
  :where(.cajetadoc .topbar-brand-row) {
    display: flex; align-items: center; gap: 1rem;
  }
  /* brand: icon (leftmost) + title, left-justified */
  :where(.cajetadoc .brand) {
    display: inline-flex; align-items: center; gap: 0.6rem;
    color: var(--cajetadoc-fg); text-decoration: none; font-weight: 700;
  }
  :where(.cajetadoc .brand:hover) { text-decoration: none; }
  :where(.cajetadoc .brand-icon) {
    inline-size: 1.75rem; block-size: 1.75rem; border-radius: 0.45rem;
    display: block; flex: none;
  }
  :where(.cajetadoc .brand-title) { font-size: 1.15rem; letter-spacing: 0.01em; }
  /* project meta: version / date / license, right-justified */
  :where(.cajetadoc .project-meta) {
    margin-inline-start: auto;
    display: flex; align-items: center; gap: 0.5rem; flex-wrap: wrap;
    justify-content: flex-end;
    color: var(--cajetadoc-muted); font-size: 0.82rem;
    font-family: var(--cajetadoc-mono);
  }
  :where(.cajetadoc .project-meta .meta-sep) { opacity: 0.5; }
  :where(.cajetadoc .project-meta .meta-license) {
    border: 1px solid var(--cajetadoc-border); border-radius: 999px;
    padding-inline: 0.5em; padding-block: 0.05em;
  }
  /* Header second row: just the breadcrumbs. */
  :where(.cajetadoc .topbar-nav-row) {
    display: flex; align-items: center; margin-block-start: 0.4rem;
  }

  /* breadcrumbs */
  :where(.cajetadoc .crumbs) {
    font-size: 0.85em; color: var(--cajetadoc-muted); min-inline-size: 0;
    overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
  }
  :where(.cajetadoc .crumbs .sep) { margin-inline: 0.35em; opacity: 0.6; }
  :where(.cajetadoc .crumbs .crumb-current) { color: var(--cajetadoc-fg); font-weight: 600; }

  /* Two-column page: left side nav (drill-down child navigation) + content.
     The side nav sticks just below the fixed header. */
  :where(.cajetadoc .page) {
    display: grid;
    grid-template-columns: var(--cajetadoc-rail) minmax(0, 1fr);
    column-gap: var(--cajetadoc-rail-gap);
    align-items: start;
  }
  :where(.cajetadoc .content) { min-inline-size: 0; max-inline-size: 75rem; }
  /* Pages with no side nav (overview / package) align with the content column. */
  :where(.cajetadoc .doc-body > .content) { max-inline-size: 75rem; }
  /* section headings get an underline */
  :where(.cajetadoc .content > h2) {
    border-block-end: 1px solid var(--cajetadoc-border);
    padding-block-end: 0.3rem;
  }
  :where(.cajetadoc .sidebar) {
    position: sticky; inset-block-start: calc(var(--cajetadoc-header-h) + 0.5rem);
    max-block-size: calc(100vh - var(--cajetadoc-header-h) - 1rem); overflow-y: auto;
  }
  /* prev/next pager at the top of the side nav: two icon-only arrow buttons. */
  :where(.cajetadoc .pager) {
    display: flex; gap: 0.4rem; margin-block-end: 0.85rem;
  }
  :where(.cajetadoc .pager-btn) {
    display: inline-flex; align-items: center; justify-content: center;
    inline-size: 2rem; block-size: 2rem;
    border: 1px solid var(--cajetadoc-border); border-radius: var(--cajetadoc-radius);
    color: var(--cajetadoc-fg); background: var(--cajetadoc-bg);
  }
  :where(.cajetadoc a.pager-btn:hover) {
    border-color: var(--cajetadoc-accent); color: var(--cajetadoc-accent);
    background: var(--cajetadoc-code-bg); text-decoration: none;
  }
  :where(.cajetadoc .pager-btn.disabled) {
    color: var(--cajetadoc-muted); opacity: 0.4; cursor: default;
  }
  :where(.cajetadoc .pager-ico) { inline-size: 1rem; block-size: 1rem; display: block; }
  /* side nav (vertical drill-down type list) */
  :where(.cajetadoc .pkg-nav-title) {
    font-size: 0.8em; color: var(--cajetadoc-muted); margin-block-end: 0.4rem;
    text-transform: lowercase;
  }
  :where(.cajetadoc .pkg-nav ul) { list-style: none; margin: 0; padding: 0; }
  :where(.cajetadoc .pkg-nav li) { margin-block: 0.05rem; font-size: 0.9em; }
  :where(.cajetadoc .pkg-nav li > a) {
    display: block; padding: 0.15rem 0.5rem; border-radius: 0.3rem;
    border-inline-start: 2px solid transparent;
  }
  :where(.cajetadoc .pkg-nav li > a:hover) { background: var(--cajetadoc-code-bg); text-decoration: none; }
  /* selection highlight: where the user currently is */
  :where(.cajetadoc .pkg-nav li.active > a) {
    font-weight: 700; color: var(--cajetadoc-fg);
    background: var(--cajetadoc-code-bg);
    border-inline-start-color: var(--cajetadoc-accent);
  }
  :where(.cajetadoc .pkg-nav .kind) { font-size: 0.7em; opacity: 0.7; }

  /* On narrow viewports let the project meta wrap under the brand. */
  @container (max-width: 40rem) {
    :where(.cajetadoc .project-meta) {
      margin-inline-start: 0; inline-size: 100%; justify-content: flex-start;
    }
    :where(.cajetadoc .topbar-brand-row) { flex-wrap: wrap; }
  }
  /* Collapse the side nav above the content on narrow containers. */
  @container (max-width: 56rem) {
    :where(.cajetadoc .page) { display: block; }
    :where(.cajetadoc .sidebar) {
      position: static; max-block-size: none; margin-block-end: 1.5rem;
      border-block-end: 1px solid var(--cajetadoc-border); padding-block-end: 1rem;
    }
  }
  @container (max-width: 34rem) {
    :where(.cajetadoc .summary .msig, .cajetadoc .params .pname) { white-space: normal; }
  }
}
)CSS";
}

} // namespace cajetadoc
