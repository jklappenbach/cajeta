//
// Front-matter Markdown splitting + parsing.
// See specs/archive/yaml-frontmatter-spec.md.
//
#pragma once

#include <string>
#include <string_view>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

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

    // A parsed front-matter Markdown document (spec §4.1): the YAML header as an
    // `llvm::json::Value` and the Markdown body verbatim.
    struct FrontMatter {
        // Parsed frontmatter. An empty object `{}` when the document has no
        // frontmatter fence.
        llvm::json::Value frontmatter = llvm::json::Object{};
        // Markdown body, byte-for-byte.
        std::string body;
    };

    // Parse a front-matter Markdown document: split off the `---` header
    // (splitFrontMatter), parse it as YAML (parseYaml), and return it alongside
    // the verbatim body. With no frontmatter, the value is `{}` and the body is
    // the whole input. Parse errors name the document-absolute line.
    llvm::Expected<FrontMatter> parseFrontMatter(std::string_view source);

    // Like parseFrontMatter, but reads `path` from disk. I/O and parse errors
    // carry the file path (spec uc 4.2.2).
    llvm::Expected<FrontMatter> parseFrontMatterFile(llvm::StringRef path);

} // namespace cajeta::buildtool
