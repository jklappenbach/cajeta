// Compiler-diagnostic helpers for the --diag-hints feature
// (docs/CompilerModes.md § --diag-hints). When the flag is on,
// error and warning messages that name an unresolved identifier are
// enriched with "did you mean..." suggestions drawn from contextually
// available names. When the flag is off, callers skip the lookup
// entirely and emit lean error text.
//
// Surface is intentionally small: each helper is pure, takes its
// inputs by reference, and returns the result by value. Gating on
// the diagHints flag happens at the call site (so off-mode callers
// don't even invoke the distance computation).
//
// Distance metric is Levenshtein (single-character edit distance);
// matches at distance ≤ 2 are typically typo-shaped without
// over-reaching into unrelated names.

#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "Exception.h"

namespace antlr4 { class Token; }

namespace cajeta {

    // Build a cajeta::Exception carrying `token`'s source location (1-based
    // line/column) plus the active module's source path — for
    // located-semantic-diagnostics. A null token yields an unlocated Exception.
    Exception locatedException(antlr4::Token* token,
                               const std::string& message,
                               const std::string& errorId);


    // Compute the Levenshtein edit distance between two strings.
    // O(|a| · |b|) time and O(min(|a|, |b|)) space. Distance includes
    // the conventional insert / delete / substitute operations, each
    // weighted 1.
    int levenshteinDistance(const std::string& a, const std::string& b);

    // Pick names from `candidates` that are close to `target`. Returns
    // a sorted list of suggestions by ascending distance, ties broken
    // alphabetically. Filters: distance ≤ maxDistance, returns at most
    // maxSuggestions entries. Empty result if no candidate is close
    // enough — caller should skip the "did you mean..." preamble in
    // that case.
    std::vector<std::string> pickSimilar(
        const std::string& target,
        const std::vector<std::string>& candidates,
        int maxDistance = 2,
        std::size_t maxSuggestions = 3);

    // Format a list of suggestions into a "did you mean..." suffix
    // appropriate to append to an error message. Empty input → empty
    // string (so the caller can string-concat unconditionally and
    // get a clean message when there are no good matches). Single
    // suggestion: "did you mean `foo`?". Multiple: "did you mean one
    // of: `foo`, `bar`, `baz`?". Backticks frame each name so they
    // survive line-wrapping cleanly in terminal output.
    std::string formatDidYouMean(const std::vector<std::string>& suggestions);

    // Emit one machine-readable diagnostic as a single self-contained NDJSON
    // line to stderr — the payload of `--diag-format=json` (docs/CompilerModes.md
    // § --diag-format). Fields: severity ("error" | "warning" | "note"), code
    // (compiler error id; "" → JSON null), message, file (source path; "" → JSON
    // null), line / column (1-based; ≤ 0 → JSON null). One line per call so a
    // consumer (the IDE plugin, build tool) can parse diagnostics incrementally
    // as they stream.
    void emitJsonDiagnostic(const std::string& severity,
                            const std::string& code,
                            const std::string& message,
                            const std::string& file = "",
                            int line = -1,
                            int column = -1);

} // namespace cajeta
