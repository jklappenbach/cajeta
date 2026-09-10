// A parsed, validated skill document: front-matter Markdown, whose body is the
// agent-facing payload. A skill is versioned with its library, never alone.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

namespace cajeta::buildtool::skill {

    struct SkillDocument {
        std::string id;
        // The canonical names this skill aids: required, and no entry empty.
        std::vector<std::string> appliesTo;
        // Optional, "" when absent; also indexed for fuzzy title search.
        std::string title;
        std::string description;
        std::string body;

        // Parse front-matter Markdown into a validated document; `sourceName` is
        // for diagnostics only. Malformed YAML or a missing field is an error.
        static llvm::Expected<SkillDocument>
        parse(std::string_view source, llvm::StringRef sourceName = "<skill>");

        // The same, from disk; errors carry the path.
        static llvm::Expected<SkillDocument> parseFile(llvm::StringRef path);

        // Validate required fields, naming the offending one; parse() calls it.
        llvm::Error validate(llvm::StringRef sourceName) const;
    };

} // namespace cajeta::buildtool::skill
