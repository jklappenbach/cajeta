---
id: search-fuzzy-Matcher
applies-to: [cajeta/search/fuzzy/Matcher]
title: Matcher<T> — typo-tolerant key→value fuzzy lookup
description: Build an in-memory fuzzy index with add(key,value) and rank typo-tolerant hits with match(query)
---

# Matcher\<T\>

The **main access point** for fuzzy (typo-tolerant) lookup in `cajeta.search.fuzzy`:
you construct one, `add` your `(key, value)` pairs, then call `match(query)` to get
the stored values whose keys are close to the query, ranked closest-first. Generic
over the value type `T`. Use it whenever you want "find what the user *meant*" rather
than an exact map lookup.

It composes the lower layers for you — `ngram.Index` (recall prefilter) and
`distance.Distance.damerau` (scoring). You normally never touch those directly; reach
for them only to build a different ranking policy.

## Construct

`heap Matcher<T>()` — the only constructor; takes no arguments and starts empty. It
internally builds a **bigram** `Index` (n=2). Bigrams, not trigrams, on purpose: one
adjacent transposition (the common typo `Fiel`→`File`) destroys *every* trigram of a
short key but leaves a shared bigram, so bigram recall survives exactly the distance-1
typos `damerau` scores. There is no capacity hint, no comparator, and no n-gram knob.

## Methods that matter

- `void add(String key, T value)` — index `value` under `key`. The Matcher takes an
  **owned internal copy** of `key` (it substrings it), so your `key` is borrowed for
  the call only; you keep ownership of what you passed. `value` is stored by value.
  Duplicate keys are *not* deduplicated — each `add` is a distinct entry.
- `#ArrayList<Match<T>> match(String query)` — returns a **freshly heap-allocated,
  owned** list (the `#` return): the caller owns it and the `Match<T>` elements inside.
  Each `Match` owns its own copy of the key, so the result list **outlives the
  Matcher** that produced it. Ranked by ascending edit distance, then key
  ascending (byte order) as a tiebreak — so identical queries always produce identical
  output (exact matches, distance 0, rank first). Returns an **empty list, never null**,
  when nothing is close enough.

See `cajeta/search/fuzzy/Match` for the result element (`value()`, `key()`,
`distance()`).

## Threshold — what gets dropped

`match` keeps a candidate only if its Damerau distance to the query is `<=` a
length-scaled threshold of `query.count() / 2`, floored at 1. So a short query is
strict (distance 1) and longer queries tolerate more typos. Two ways a key is absent
from results, by design — know these to avoid a dead-end debugging the index:

- **Past threshold:** shares a bigram but is too far (e.g. query `Fiel` vs `Fibonacci`).
- **No shared bigram:** never even prefiltered as a candidate (e.g. `Fiel` vs `Network`).

## State & lifecycle

Mutable and **reusable**: interleave `add` and `match` freely, and call `match` any
number of times. Not documented thread/fiber-safe — confine to one fiber or guard it.
It is a plain heap object with no `close()`/dispose step; it drops with its owner.

## Example (mirrors test/search/FuzzyMatcherTests.cpp)

```cajeta
import cajeta.search.fuzzy.Matcher;
import cajeta.search.fuzzy.Match;
import cajeta.collection.ArrayList;

Matcher<int32> m = heap Matcher<int32>();
m.add("File", 10);
m.add("Files", 11);
m.add("Fibonacci", 55);   // shares "fi" but past threshold -> dropped
m.add("Network", 99);     // no shared bigram -> never a candidate

ArrayList<Match<int32>> hits = m.match("Fiel");  // caller owns hits + elements
Match<int32> top = hits.get(0);                  // -> value 10, distance 1
// hits.count() == 2 (File, Files)
```
