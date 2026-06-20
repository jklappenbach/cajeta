//
// YAML-header parser for front-matter Markdown. Covers the frontmatter-relevant
// subset only (comments, mappings, scalars, sequences, indentation nesting) —
// NOT a general YAML 1.2 engine. See docs/specs/yaml-frontmatter-spec.md §3.
//
#pragma once

#include <string_view>

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

namespace cajeta::buildtool {

    // Parse a YAML frontmatter header (the text between the `---` fences, already
    // split out by splitFrontMatter) into an `llvm::json::Value`.
    //
    // Supported (spec §3.1): `#` comments and blank lines; `key: value` mappings
    // nested by space indentation; plain / single- / double-quoted scalars typed
    // as bool, null, number, or string (a quoted scalar is always a string).
    // Block and flow sequences are added in unit D.Y3.
    //
    // An empty (or all-comment) header parses to an empty object `{}`. Parse
    // failures (unterminated quote, tab indentation, bad structure) return an
    // error naming the 1-based line.
    llvm::Expected<llvm::json::Value> parseYaml(std::string_view header);

} // namespace cajeta::buildtool
