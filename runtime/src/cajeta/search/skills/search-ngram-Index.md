---
id: search-ngram-Index
applies-to: [cajeta/search/ngram/Index]
title: Index<T> — n-gram inverted prefilter for fuzzy candidate generation
description: Recall-oriented trigram index; add(key,value)/candidates(query) returns a deduped owned superset to score with edit distance — not a collection.
---

# `Index<T>` — n-gram inverted prefilter

A **recall-oriented prefilter**, not a collection. Build it, `add` key→value pairs,
then call `candidates(query)` to get **every value whose key shares at least one
n-gram with the query, deduplicated by value**. The result is a *superset*: you (or
`cajeta.search.fuzzy.Matcher`) then rank it with an edit distance. If you want exact
lookup or storage, use `cajeta.collection.HashMap` instead — this type only answers
"what might match".

Package `cajeta.search.ngram`. It is an **access point** — you construct it directly.

## Construct

```cajeta
import cajeta.search.ngram.Index;
import cajeta.collection.ArrayList;

Index<int32> idx = heap Index<int32>();    // trigrams (n = 3, default)
Index<int32> bi  = heap Index<int32>(2);   // bigrams, or any gram size
```

Two constructors: `Index()` (trigrams) and `Index(int32 gramSize)`. Both pre-size the
backing `HashMap` to 64. There is **no** `close()`/`dispose()` and no explicit
disposal protocol — the instance and everything it owns are reclaimed when the heap
handle drops.

## The two methods that matter

- `void add(String key, T value)` — index `value` under every n-gram of `key`. Both
  args are **borrowed** at the call site (no `#`). Internally the index keeps its own
  copy of each gram key (a fresh `substring`) and a bucket, so `key` need not outlive
  the call; `value` is stored by value into the bucket. Adding the same value twice is
  harmless — `candidates` dedups.
- `#ArrayList<T> candidates(String query)` — **owned return**: the caller owns the
  `ArrayList<T>` and must free it. `query` is borrowed. Empty list when nothing shares
  a gram (never null).

```cajeta
idx.add("apple", 1);
idx.add("zebra", 2);
ArrayList<int32> hits = idx.candidates("aple");  // → [1]; shares "ple", "zebra" shares nothing
```

## Gram rules (drive what matches)

- **Lowercased** for recall — matching is case-insensitive.
- A key/query **shorter than `n`** is indexed/queried as a **single gram** (the whole
  lowercased string). So `add("io", 9)` is found by `candidates("io")`.
- An **empty** key/query yields no grams (and so never matches / indexes nothing).
- Gram size must match across `add` and `candidates` on the same index — that's why
  it's set once in the constructor.

## What it does NOT do

- **Not a collection** — no `get(key)`, `remove`, `size`, or iteration over entries.
  It is built *over* `HashMap`/`ArrayList`/`HashSet` but exposes only the
  add/candidates retrieval contract.
- **Does not score or rank** — `candidates` returns an unranked, deduped superset.
  Apply an edit distance (`cajeta.search.distance`) or `cajeta.search.fuzzy.Matcher`
  to the result.
- **Does not deduplicate keys** — calling `add` with the same key/value pair again
  just appends to buckets; recall is unaffected because `candidates` dedups by value.

## Concurrency

Mutable; `add` mutates the backing map. Not thread/fiber-safe — guard external
synchronization if shared.

See `cajeta.search.fuzzy.Matcher` for the scoring stage. Verified by
`test/search/NgramIndexTests.cpp`.
