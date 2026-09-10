// Front-matter Markdown splitting + parsing.
// See specs/archive/yaml-frontmatter-spec.md.
#pragma once

#include <string>
#include <string_view>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

namespace cajeta::buildtool {

    // A split document: raw text either side of the fence, nothing parsed.
    struct FrontMatterSplit {
        bool present = false;   // the document began with a `---` fence
        std::string header;     // YAML between the fences, fences excluded
        std::string body;       // after the closing fence, or the whole input
    };

    // Split a front-matter Markdown document. A leading `---` (after an optional
    // BOM) opens the header, which runs to the next line that is exactly `---` or
    // `...`; both halves keep their bytes. An unclosed opening fence is an error.
    llvm::Expected<FrontMatterSplit> splitFrontMatter(std::string_view source);

    struct FrontMatter {
        llvm::json::Value frontmatter = llvm::json::Object{};   // `{}` when unfenced
        std::string body;                                       // byte-for-byte
    };

    // Split off the `---` header, parse it as YAML, and return it alongside the
    // verbatim body; with no frontmatter the value is `{}` and the body is the
    // whole input. Parse errors name the document-absolute line.
    llvm::Expected<FrontMatter> parseFrontMatter(std::string_view source);

    // Like parseFrontMatter, but reads `path` from disk; errors carry the path.
    llvm::Expected<FrontMatter> parseFrontMatterFile(llvm::StringRef path);

} // namespace cajeta::buildtool
