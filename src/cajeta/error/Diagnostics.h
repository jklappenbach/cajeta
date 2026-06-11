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

namespace cajeta {

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

} // namespace cajeta
