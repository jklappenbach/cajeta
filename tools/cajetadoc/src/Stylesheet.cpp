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
    /* relaxed, left-anchored layout: use the viewport width with responsive
       gutters instead of a narrow centered column. */
    padding-inline: clamp(1rem, 3vw, 2.75rem);
    padding-block: 1.5rem 3rem;
    container-type: inline-size;
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
  /* breadcrumbs */
  :where(.cajetadoc .crumbs) {
    font-size: 0.85em; color: var(--cajetadoc-muted); margin-block-end: 1rem;
  }
  :where(.cajetadoc .crumbs .sep) { margin-inline: 0.35em; opacity: 0.6; }
  :where(.cajetadoc .crumbs .crumb-current) { color: var(--cajetadoc-fg); font-weight: 600; }
  /* Symmetric, reactive layout.
     Type pages: [left rail | content]. The content fills to the right page
     edge, but its body elements are inset on the right by --cajetadoc-inset so
     the right margin mirrors the left rail. Heading rules break out of that
     inset to span the full width. The middle expands as the window grows; both
     rails stay a fixed width. */
  :where(.cajetadoc .page) {
    display: grid;
    grid-template-columns: var(--cajetadoc-rail) minmax(0, 1fr);
    column-gap: var(--cajetadoc-rail-gap);
    align-items: start;
  }
  :where(.cajetadoc .content) {
    min-inline-size: 0;
    /* inset body content on the right to mirror the left rail */
    padding-inline-end: var(--cajetadoc-inset);
  }
  /* Pages with no sidebar (overview / package): mirror the rail offset on the
     left too so their content aligns with the type pages. */
  :where(.cajetadoc > .content) { margin-inline-start: var(--cajetadoc-inset); }
  /* Heading rules + horizontal dividers span past the right inset to the margin. */
  :where(.cajetadoc .content > .type-header,
         .cajetadoc .content > h2,
         .cajetadoc .content > hr,
         .cajetadoc .content > .prevnext,
         .cajetadoc .content > .cajetadoc-footer) {
    margin-inline-end: calc(-1 * var(--cajetadoc-inset));
  }
  :where(.cajetadoc .sidebar) {
    position: sticky; inset-block-start: 1rem;
    max-block-size: calc(100vh - 2rem); overflow-y: auto;
  }
  /* section headings get an underline that runs to the right margin */
  :where(.cajetadoc .content > h2) {
    border-block-end: 1px solid var(--cajetadoc-border);
    padding-block-end: 0.3rem;
  }
  :where(.cajetadoc .pkg-nav-title) {
    font-size: 0.8em; color: var(--cajetadoc-muted); margin-block-end: 0.4rem;
    text-transform: lowercase;
  }
  :where(.cajetadoc .pkg-nav ul) { list-style: none; margin: 0; padding: 0; }
  :where(.cajetadoc .pkg-nav li) { margin-block: 0.1rem; font-size: 0.9em; }
  :where(.cajetadoc .pkg-nav li.active > a) { font-weight: 700; color: var(--cajetadoc-fg); }
  :where(.cajetadoc .pkg-nav .kind) { font-size: 0.7em; opacity: 0.7; }
  /* prev/next */
  :where(.cajetadoc .prevnext) {
    display: flex; justify-content: space-between; gap: 1rem; margin-block-start: 2rem;
    padding-block-start: 1rem; border-block-start: 1px solid var(--cajetadoc-border);
  }
  :where(.cajetadoc .prevnext .next) { margin-inline-start: auto; }
  /* collapse the rails on narrow containers: stack the nav, drop the insets so
     content uses the full width. */
  @container (max-width: 56rem) {
    :where(.cajetadoc .page) { display: block; }
    :where(.cajetadoc .sidebar) {
      position: static; max-block-size: none; margin-block-end: 1.5rem;
      border-block-end: 1px solid var(--cajetadoc-border); padding-block-end: 1rem;
    }
    :where(.cajetadoc .content) { padding-inline-end: 0; }
    :where(.cajetadoc > .content) { margin-inline-start: 0; }
    :where(.cajetadoc .content > .type-header,
           .cajetadoc .content > h2,
           .cajetadoc .content > hr,
           .cajetadoc .content > .prevnext,
           .cajetadoc .content > .cajetadoc-footer) { margin-inline-end: 0; }
  }
  @container (max-width: 34rem) {
    :where(.cajetadoc .summary .msig, .cajetadoc .params .pname) { white-space: normal; }
  }
}
)CSS";
}

} // namespace cajetadoc
