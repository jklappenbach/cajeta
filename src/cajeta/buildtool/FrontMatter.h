//
// Front-matter Markdown splitting + parsing.
// See docs/specs/yaml-frontmatter-spec.md.
//
#pragma once

#include <string>
#include <string_view>

#include <llvm/Support/Error.h>

namespace cajeta::buildtool {

    // Result of splitting a front-matter Markdown document into its YAML header
    // and Markdown body (spec §2). The header/body are returned as raw text; no
    // YAML or Markdown is parsed here.
    struct FrontMatterSplit {
        // True iff the document began with a `---` frontmatter fence.
        bool present = false;
        // YAML header text between the fences (fences excluded), byte-for-byte.
        // Empty when !present.
        std::string header;
        // Markdown body: everything after the closing fence, byte-for-byte; or the
        // entire input when !present.
        std::string body;
    };

    // Split a front-matter Markdown document (spec §2).
    //
    // A leading `---` line (after an optional UTF-8 BOM) opens the YAML header,
    // which runs to the next line that is exactly `---` or `...`; the body is
    // everything after that closing fence, preserved byte-for-byte (including its
    // line ending). With no leading fence, returns {present=false, header="",
    // body=source}. An opening `---` with no closing fence is an error.
    llvm::Expected<FrontMatterSplit> splitFrontMatter(std::string_view source);

} // namespace cajeta::buildtool
