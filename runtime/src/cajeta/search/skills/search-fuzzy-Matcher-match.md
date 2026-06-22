---
id: search-fuzzy-Matcher-match
applies-to: [cajeta/search/fuzzy/Matcher.match]
title: Matcher.match — ranked typo-tolerant lookup, caller owns the result list
description: match(query) returns an owned ArrayList<Match<T>> ranked by distance then key; length-scaled threshold, empty when nothing is close enough.
---

# `Matcher<T>.match`

```cajeta
public #ArrayList<Match<T>> match(String query)
```

Returns **every stored value whose key is within an edit-distance threshold of
`query`**, ranked closest-first. Call this after populating the matcher with
`add(key, value)`. This is the read side of `Matcher`; see the class for
construction and indexing.

## What you get back, and who owns it

- The return is `#ArrayList<Match<T>>` — **ownership transfers to the caller**.
  You must free it (and it owns its `Match<T>` elements). It is independent of
  the `Matcher`: each `Match` holds its own copied key, so the list outlives the
  `Matcher` that produced it.
- **Ranking is deterministic**: ascending `distance`, ties broken by ascending
  key (byte-wise). Identical inputs always yield identical output; an exact match
  (distance 0) always ranks first.
- **Empty when nothing qualifies** — never null. A query whose candidates are all
  past threshold, or that shares no bigram with any key, yields a length-0 list.

## The threshold (the load-bearing gotcha)

The cutoff is **`query.count() / 2`, clamped to a minimum of 1** — it scales with
the *query* length, not the key length. So short queries are strict: a 3-char
query tolerates distance 1; an 8-char query tolerates up to 4. A candidate is
kept only when `Distance.damerau(query, key) <= threshold`. This is why a key
that merely shares a gram (e.g. `"Fibonacci"` for query `"Fiel"`) is dropped —
it survives the prefilter but fails the distance check.

## What it does NOT do

- Does **not** mutate the matcher — read-only, call it repeatedly.
- Does **not** return keys that share no bigram with the query: such keys never
  reach the distance check (recall is bounded by the underlying bigram
  `ngram.Index`). It finds typos, not substring/semantic matches.
- Does **not** dedup by value or cap the result count — every qualifying entry is
  returned.
- Does **not** borrow into the matcher's storage — each `Match.key()` is a copy.

## Example (mirrors test/search/FuzzyMatcherTests.cpp)

```cajeta
import cajeta.search.fuzzy.Matcher;
import cajeta.search.fuzzy.Match;
import cajeta.collection.ArrayList;

Matcher<int32> m = heap Matcher<int32>();
m.add("File", 10);
m.add("Files", 11);
m.add("Fibonacci", 55);          // shares "fi" but distance > threshold → dropped
m.add("Network", 99);            // no shared bigram → never a candidate

ArrayList<Match<int32>> r = m.match("Fiel");   // caller owns r
// r == [ Match(File, 10, distance 1), Match(Files, 11, distance 2) ]
Match<int32> top = r.get(0);
int32 v = top.value();           // 10
int32 d = top.distance();        // 1
```

See `cajeta.search.fuzzy.Match` for the result element, and
`cajeta.search.distance.Distance.damerau` for the scoring (OSA: one adjacent
transposition costs 1).
