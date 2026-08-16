---
id: search-fuzzy-overview
applies-to: [cajeta/search/fuzzy]
title: Fuzzy neighborhood — typo-tolerant key→value lookup (Matcher + Match)
description: Use Matcher<T> for ranked typo-tolerant lookup; Match<T> is the owned result. Recall via ngram.Index, scoring via Distance.damerau.
---

# cajeta.search.fuzzy — typo-tolerant lookup

**Responsibility:** the assembled fuzzy lookup. This package owns the *composition* —
turning "find keys close to a query" into a ranked result. It does not own the
algorithms: recall (candidate generation) comes from `cajeta.search.ngram.Index` and
scoring from `cajeta.search.distance.Distance`. If you only need raw candidate sets or a
bare edit-distance number, go to those packages directly; come here when you want the
whole thing wired together and ranked.

## Inventory

**Entry-point type (instantiate this):**
- `Matcher<T>` — the one class you build and call. `add(key, value)` to populate,
  `match(query)` to look up. `T` is your value/payload type.

**Support / result type (you receive these, never construct them):**
- `Match<T>` — one result row: `value()`, `key()`, `distance()`. Returned inside the
  list from `Matcher.match`.

## Collaboration

`Matcher.match(query)` runs a two-stage pipeline:

1. **Recall** — `ngram.Index<int32>` (built with **bigrams**, n=2) returns candidate
   entry ids whose key shares at least one bigram with the query. Bigrams (not the
   default trigrams) are deliberate: a single adjacent transposition like `Fiel`→`File`
   destroys every trigram of a short key but leaves a shared bigram, so recall survives
   exactly the typos `damerau` scores as distance 1.
2. **Score + rank** — each candidate's stored key is scored with
   `Distance.damerau(query, key)` (OSA edit distance), dropped if past a length-scaled
   threshold (`query.length / 2`, minimum 1), and the survivors are returned ordered by
   **ascending distance, then key (byte-lexicographic) tiebreak**. So an exact match
   (distance 0) always ranks first, and identical inputs always produce identical output.

A key that shares no bigram with the query is never even a candidate (not just dropped) —
e.g. `"Network"` for query `"Fiel"`.

## Worked example

```cajeta
import cajeta.search.fuzzy.Matcher;
import cajeta.search.fuzzy.Match;
import cajeta.collection.ArrayList;

Matcher<int32> m = heap Matcher<int32>();
m.add("File", 10);
m.add("Files", 11);
m.add("Fibonacci", 55);          // shares "fi" but scores past threshold → dropped
m.add("Network", 99);            // shares no bigram → never a candidate

ArrayList<Match<int32>> hits #= m.match("Fiel");   // ranked closest-first
Match<int32> top = hits.get(0);
// top.key() == "File", top.value() == 10, top.distance() == 1
```

## Ownership & lifecycle

- `add(String key, T value)` — `key` is **borrowed**: `Matcher` immediately makes its own
  owned copy (a `substring`), so the caller keeps ownership of the string it passed and
  may free or mutate it afterward. `value` is stored by copy.
- `match(query)` returns `#ArrayList<Match<T>>` — an **owned** list; the **caller owns and
  frees it**. The `query` argument is borrowed.
- Each `Match<T>` **owns its own copy of the key**, so the returned list (and every
  `Match` in it) **outlives the `Matcher`** that produced it. You can drop the `Matcher`
  and keep the results.
- An empty list (not null) means nothing was close enough. `match` never returns null.

## What this package does NOT do

- **Not a collection / not mutable after-the-fact.** There is no `remove`, `update`,
  `contains`, `size`, or key dedup. `add` is append-only; adding the same key string
  twice creates **two independent entries** (both can appear in results).
- **No persistence, no scoring knobs.** The bigram size, the `damerau` choice, and the
  `length/2` threshold are fixed in `Matcher`; there is no configuration surface. If you
  need a different distance, threshold, or n-gram size, compose `ngram.Index` and
  `Distance` yourself.
- **Byte-level, not Unicode.** Matching is over `char` (byte) surface; fine for
  identifier/title keys, not Unicode-collated.

## See also

- `cajeta/search/ngram` (`Index<T>`) — the recall prefilter, for raw candidate sets.
- `cajeta/search/distance` (`Distance`) — `damerau` / `levenshtein` scoring, for a bare
  edit-distance number.
