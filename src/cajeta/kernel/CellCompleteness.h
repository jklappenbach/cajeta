// The `is_complete_request` triage (jupyter-kernel spec 3.4): submit the cell, or
// open another line? Purely syntactic — a first syntax error whose offending token
// is EOF means incomplete, an error elsewhere invalid, and no error complete.
#pragma once

#include <string>

namespace cajeta::kernel {

    enum class Completeness { Complete, Incomplete, Invalid };

    const char* completenessName(Completeness c);

    // `indent` (non-null) receives the continuation indent for an INCOMPLETE
    // verdict — four spaces per unclosed brace, empty for every other verdict.
    Completeness classifyCell(const std::string& source,
                              std::string* indent = nullptr);

}  // namespace cajeta::kernel
