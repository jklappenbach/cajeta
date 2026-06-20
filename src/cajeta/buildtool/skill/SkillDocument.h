//
// A parsed, validated skill document (skill-discovery spec §4). A skill is
// front-matter Markdown: a YAML header (id, applies-to, title, description)
// plus a Markdown body that is the agent-facing payload. There is no per-skill
// version — a skill is versioned with its library (spec §3.4).
//
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <llvm/ADT/StringRef.h>
#include <llvm/Support/Error.h>

namespace cajeta::buildtool::skill {

    // The machine-facing metadata + the Markdown payload of one skill.
    struct SkillDocument {
        // Unique skill id within its library (required).
        std::string id;
        // Canonical-name bindings this skill aids — library/package/class/method
        // (spec §3.1). Required, non-empty, every entry non-empty.
        std::vector<std::string> appliesTo;
        // Short human-facing summary (optional; "" when absent). Also indexed for
        // fuzzy title search (spec §3.5).
        std::string title;
        // Longer summary (optional; "" when absent).
        std::string description;
        // Markdown body — the implementation guidance payload, byte-for-byte.
        std::string body;

        // Parse front-matter Markdown into a validated SkillDocument. `sourceName`
        // is used only for diagnostics (a path or logical name). Returns an error
        // for malformed YAML, a missing frontmatter block, or missing/empty
        // required fields.
        static llvm::Expected<SkillDocument>
        parse(std::string_view source, llvm::StringRef sourceName = "<skill>");

        // Like parse(), but reads `path` from disk; I/O and parse errors carry the
        // path.
        static llvm::Expected<SkillDocument> parseFile(llvm::StringRef path);

        // Validate required fields, reporting `sourceName` + the offending field.
        // parse()/parseFile() call this; exposed for callers holding a document.
        llvm::Error validate(llvm::StringRef sourceName) const;
    };

} // namespace cajeta::buildtool::skill
