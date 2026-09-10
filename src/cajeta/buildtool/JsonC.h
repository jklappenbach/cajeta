// JSONC parsing for build-tool manifests: strict JSON plus `//` and `/* */`
// comments and trailing commas. A single-pass preprocessor blanks those to
// same-length whitespace, so llvm::json's error locations still line up.

#pragma once

#include <llvm/Support/JSON.h>
#include <string>
#include <string_view>

namespace cajeta::buildtool {

    // Strip comments and trailing commas, returning a string of the SAME length:
    // stripped characters become spaces, or newlines inside a `//` comment.
    std::string preprocessJsonC(std::string_view source);

    // Parse a JSONC string; a failure's location refers to the original source.
    llvm::Expected<llvm::json::Value> parseJsonC(std::string_view source);

    // The same, from disk; errors name the path.
    llvm::Expected<llvm::json::Value> parseJsonCFile(const std::string& path);

} // namespace cajeta::buildtool
