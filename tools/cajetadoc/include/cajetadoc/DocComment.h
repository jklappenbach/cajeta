// cajetadoc — doc-comment parsing (plan §3).
//
// Parse raw `/** */` text into a doc-AST: strip the leading `*` gutter,
// separate the summary (first sentence) from the body, split the trailing
// contiguous block-tag section (`@Param …`) from the body. Inline tags
// (`{@Link …}`) are left embedded in summary/body text and expanded by the
// renderer (§7).
#ifndef CAJETADOC_DOCCOMMENT_H
#define CAJETADOC_DOCCOMMENT_H

#include <string>
#include <vector>

namespace cajetadoc {

// A parsed block tag. For `@Param obj the object to hash`, name="Param",
// arg="obj", body="the object to hash". For tags without a leading argument
// (e.g. `@Return …`, `@Since …`) arg is empty and the whole remainder is body.
struct BlockTag {
    std::string name; // tag name without the leading '@' (original case)
    std::string arg;  // first token, for tags that take a name (Param/Throws); else ""
    std::string body; // Markdown body of the tag (may span multiple lines)
};

struct DocDiagnostic {
    std::string code;    // e.g. "unterminated-inline-tag", "empty-comment"
    std::string message;
    int line = 0;        // 1-based, relative to comment start
};

struct DocComment {
    std::string summary;            // first sentence (Markdown, inline tags intact)
    std::string body;               // body Markdown after the summary, before block tags
    std::vector<BlockTag> blockTags;
    std::vector<DocDiagnostic> diagnostics;
    bool empty = false;             // true when the comment had no content

    // Convenience: collect all block tags whose name matches (case-insensitive).
    std::vector<const BlockTag*> tags(const std::string& name) const;
};

// Whether a raw comment is a doc comment (`/** … */`, not `/**/` or `/* */`).
bool isDocComment(const std::string& raw);

// Parse a raw `/** */` comment (including the delimiters) into a DocComment.
DocComment parseDocComment(const std::string& raw);

// A list of the block-tag names that take a leading argument token
// (so the parser splits `arg` from `body`). Case-insensitive match.
bool tagTakesArg(const std::string& name);

} // namespace cajetadoc

#endif // CAJETADOC_DOCCOMMENT_H
