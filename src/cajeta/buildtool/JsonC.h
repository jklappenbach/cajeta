// JSONC parsing for cajeta build-tool manifest files (cajeta.json,
// task definitions, action definitions). JSONC = strict JSON's data
// model + `//` line comments + `/* */` block comments + trailing
// commas in objects and arrays.
//
// Strategy: a single-pass preprocessor strips comments and trailing
// commas while preserving line and column counts (comments are replaced
// with whitespace of the same length, not deleted, so llvm::json error
// locations still line up with the source file). The cleaned string is
// then handed to llvm::json::parse.
//
// See BuildTool.md "Manifest — cajeta.json" for the schema this
// supports. See plans/buildtool/build-tool-plan.md Phase 0 for context.

#pragma once

#include <llvm/Support/JSON.h>
#include <string>
#include <string_view>

namespace cajeta::buildtool {

    // Strip `//` line comments, `/* */` block comments, and trailing
    // commas from a JSONC source. Returns a new string of the same
    // length as the input; replaced characters become spaces (or
    // newlines, for `//` line comments) so error locations from a
    // downstream JSON parser match the original source.
    std::string preprocessJsonC(std::string_view source);

    // Parse a JSONC string. Wraps preprocessJsonC + llvm::json::parse.
    // On failure the returned Error carries a location relative to the
    // original source (the preprocessor preserves positions).
    llvm::Expected<llvm::json::Value> parseJsonC(std::string_view source);

    // Read a JSONC file from disk and parse it. Errors include the
    // path in their message.
    llvm::Expected<llvm::json::Value> parseJsonCFile(const std::string& path);

} // namespace cajeta::buildtool
