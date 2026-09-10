// YAML-header parser for front-matter Markdown: the frontmatter-relevant subset
// only (comments, mappings, scalars, sequences, indent nesting), not YAML 1.2.
// See specs/archive/yaml-frontmatter-spec.md §3.
#pragma once

#include <string_view>

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

namespace cajeta::buildtool {

    // Parses a YAML frontmatter header (the text between the `---` fences) into a
    // json::Value; empty or all-comment gives `{}`, and failures return an error
    // naming the line. `firstLine` makes those numbers document-absolute.
    llvm::Expected<llvm::json::Value> parseYaml(std::string_view header, int firstLine = 1);

} // namespace cajeta::buildtool
