---
id: search-overview
applies-to: [cajeta.search]
title: cajeta.search — typo-tolerant lookup (prefilter + score)
description: Routing for fuzzy key→value lookup; Matcher end-to-end, Index prefilter, Distance scoring, byte-level caveats and owned-return rules.
---

# cajeta.search — typo-tolerant lookup

Fuzzy, typo-tolerant key→value lookup. The core abstraction is a two-stage
pipeline: an **n-gram inverted index** generates a cheap recall *superset* of
candidates, then an **edit distance** scores and ranks them. If you want
"find values whose key is close to this query string," you are in the right
library.

## Task → entry point

| Want to… | Start with |
| --- | --- |
| End-to-end fuzzy lookup (add keys, query, get ranked hits) | `cajeta.search.fuzzy.Matcher<T>` — `add(key, value)` then `match(query)` |
| Just generate raw candidates (prefilter), score them yourself | `cajeta.search.ngram.Index<T>` — `add` / `candidates(query)` |
| Just score two strings (edit distance) | `cajeta.search.distance.Distance` — static `damerau` / `levenshtein` |
| Inspect one ranked result (value, key, distance) | `cajeta.search.fuzzy.Match<T>` (you receive these; you do not build them) |
| Unicode-correct distance / collation | **Not provided.** Distances are byte-level (see hazards) — normalize upstream. |
| Persist / serialize the index | **Not provided.** `Index` is in-memory only; rebuild by re-`add`ing. |
| Remove or update an indexed key | **Not provided.** `add` is append-only; rebuild the `Matcher`/`Index` to drop entries. |
| Configure ranking weights / fuzzy threshold | **Not exposed.** `Matcher` uses a fixed length-scaled threshold (`len(query)/2`, min 1). |

## Cross-cutting invariants

- **Ownership / `#`.** Collection-returning methods return **owned** results
  marked `#`: `Matcher.match` → `#ArrayList<Match<T>>`, `Index.candidates` →
  `#ArrayList<T>`. The caller owns and frees the returned list. `Match` objects
  inside own their own key copy, so the result list **outlives the `Matcher`**.
  When you pass owned strings across a boundary (e.g. into your own structures),
  move with `#`; `Matcher.add`/`Index.add` copy the key internally (fresh
  `substring`), so the `key` argument you pass is borrowed, not consumed.
- **No disposal protocol.** No `close()`/`dispose()`. `Matcher` and `Index` are
  plain heap objects; drop frees their internals. Owned `#` returns are the only
  thing you must account for.
- **Error style: none.** No exceptions, no `Optional`, no sentinels. A query
  with no matches returns an **empty list**, not null. `Distance` always returns
  a non-negative `int32`.
- **Determinism.** `match` is deterministic: results are ranked by ascending
  distance, ties broken by key (lexicographic, byte order). Identical inputs
  always yield identical output; exact matches (distance 0) rank first.
- **Not thread-safe.** `add` mutates internal `HashMap`/`ArrayList`; no internal
  locking. Confine a `Matcher`/`Index` to one fiber while mutating.

## End-to-end example

```cajeta
import cajeta.search.fuzzy.Matcher;
import cajeta.search.fuzzy.Match;
import cajeta.collection.ArrayList;

Matcher<int32> m = heap Matcher<int32>();
m.add("File", 10);
m.add("Files", 11);
m.add("Fibonacci", 55);   // shares a gram with "Fiel" but past threshold → dropped
m.add("Network", 99);     // no shared gram → never prefiltered

// owned list; ranked closest-first. "Fiel" → "File" at distance 1.
ArrayList<Match<int32>> hits #= m.match("Fiel");
Match<int32> top = hits.get(0);   // top.value()==10, top.key()=="File", top.distance()==1
```

## Hazards

- **Byte-level, not Unicode.** `Distance` and gram extraction operate on `char`
  (bytes) via `charAt`. Multi-byte UTF-8, case-folding beyond ASCII, and
  combining marks are **not** handled. `Index` lowercases grams (ASCII) for
  recall, but `damerau` scoring is case-*sensitive*. Good for identifier/title
  keys; normalize before indexing if you need more.
- **`damerau` is OSA, not full Damerau-Levenshtein.** Optimal String Alignment:
  a substring is edited at most once, so adjacent transpositions cost 1 but
  overlapping transpositions are not collapsed. This is the cheap, correct model
  for typo lookup — don't expect true unrestricted Damerau distance.
- **Recall is gram-bounded.** A candidate must share at least one n-gram with
  the query or it is invisible to `match`, no matter how close the edit distance.
  `Matcher` uses **bigrams (n=2)** precisely so a single transposition (`Fiel`→
  `File`) still shares a gram; a bare `Index` defaults to **trigrams (n=3)**.
- **Threshold is fixed and length-scaled.** `Matcher.match` keeps candidates
  within `len(query)/2` (minimum 1) edits. Short queries are strict (threshold
  1). You cannot tune this — use `Index` + `Distance` directly for custom scoring.
- **`Index` is not a collection.** It only answers `candidates`; there is no
  iteration, size, or lookup-by-key. Duplicate grams in one key are harmless
  (`candidates` dedups by value).

## Disambiguation

- **`Matcher` vs `Index`+`Distance`.** Use `Matcher` for ordinary fuzzy lookup —
  it wires prefilter + scoring + ranking + threshold for you. Drop to `Index`
  (prefilter) plus `Distance` (scoring) when you need a custom threshold, custom
  ranking, a different gram size, or to score candidates against something other
  than the indexed key.
- **`damerau` vs `levenshtein`.** Prefer `damerau` for typo tolerance
  (transposition = 1 edit, e.g. `flie`→`file`). Use `levenshtein` when
  transpositions should genuinely count as two edits.

## Setup

Pure runtime library, no external dependency or capability. Import the specific
type(s) you need (`cajeta.search.fuzzy.Matcher`, `cajeta.search.ngram.Index`,
`cajeta.search.distance.Distance`).

## Going deeper

Three packages: `cajeta.search.fuzzy` (`Matcher`, `Match`),
`cajeta.search.ngram` (`Index`), `cajeta.search.distance` (`Distance`). See each
type's source doc-comment for full method signatures and return semantics.
