// cajetadoc — Markdown rendering (plan §4, focused CommonMark + GFM subset).
//
// Renders doc-comment bodies and tag bodies to HTML. Supports the constructs
// that actually appear in the stdlib doc corpus: ATX headings (with demotion),
// paragraphs, emphasis/strong, inline code, fenced and indented code blocks,
// unordered/ordered lists, GFM pipe tables, blockquotes, thematic breaks,
// links and images, plus expansion of JavaDoc inline tags ({@Code}, {@Literal},
// {@Link}, {@LinkPlain}, {@Value}). All text is HTML-escaped; raw HTML in
// comments is escaped, not passed through (§4.1.6).
#ifndef CAJETADOC_MARKDOWN_H
#define CAJETADOC_MARKDOWN_H

#include <functional>
#include <string>

namespace cajetadoc {

struct MarkdownOptions {
    // Headings in a comment are demoted by this many levels so a class doc owns
    // <h2> and comment `##` becomes <h3> (§4.1.4).
    int headingOffset = 2;
    // Resolver for `{@Link target}` / `[target]` cross-references. Returns an
    // href when the target resolves, or empty to render as plain code (§5).
    std::function<std::string(const std::string& target)> linkResolver;
};

// Escape &, <, >, " for safe HTML text content.
std::string htmlEscape(const std::string& s);

// Render a Markdown block to an HTML fragment.
std::string renderMarkdown(const std::string& md, const MarkdownOptions& opts = {});

// Render a single line of Markdown inline constructs (no block structure) —
// used for summaries and table cells.
std::string renderInline(const std::string& md, const MarkdownOptions& opts = {});

} // namespace cajetadoc

#endif // CAJETADOC_MARKDOWN_H
