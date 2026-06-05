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
  :where(.cajetadoc) {
    color: var(--cajetadoc-fg);
    background: var(--cajetadoc-bg);
    font-family: var(--cajetadoc-font);
    line-height: 1.6;
    max-inline-size: 56rem;
    margin-inline: auto;
    padding-inline: 1.25rem;
    padding-block: 2rem;
    container-type: inline-size;
  }
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
  @container (max-width: 34rem) {
    :where(.cajetadoc .summary .msig, .cajetadoc .params .pname) { white-space: normal; }
  }
}
)CSS";
}

} // namespace cajetadoc
