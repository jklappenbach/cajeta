---
id: search-distance-Distance
applies-to: [cajeta/search/distance/Distance]
title: Distance — static levenshtein / damerau (OSA) edit-distance scoring
description: Pick levenshtein vs damerau for byte-level edit distance; both static, pure, symmetric, return int32 cost, no allocation crosses the boundary.
---

The scoring half of `cajeta.search` fuzzy matching: two static functions returning the
integer edit cost between two strings. **Use `damerau` for typo-tolerant lookup** (an
adjacent swap like `flie`→`file` costs 1); **use `levenshtein`** when you want classic
insert/delete/substitute only (that same swap costs 2). Both compare the strings'
`char` (byte) surface — fine for identifier- and title-style keys; this is **not**
Unicode-collation-aware (spec §1.4 non-goal).

`Distance` is a **utility class, not an access point you instantiate** — call the
methods statically, never `heap Distance()`. For end-to-end fuzzy lookup (ranking
candidates by these scores) reach for `cajeta.search.fuzzy.Matcher` instead; this class
is just the kernel it builds on.

## The two methods

```
public static int32 levenshtein(String a, String b)
public static int32 damerau(String a, String b)
```

- Return the edit cost as a plain `int32` value — a number, not a handle. **Nothing is
  owned, nullable, or freed** across the call. `0` means equal; never negative.
- `a` and `b` are **borrowed** (read-only) — no `#` transfer; the caller keeps and frees
  them as usual. The functions do not retain references to either.
- `damerau` is **OSA** (optimal string alignment), not the unrestricted
  Damerau-Levenshtein: any substring is edited at most once, so adjacent-only
  transpositions cost 1. This is the cheap, right model for fuzzy keys — do not expect
  the full-DL result for overlapping transpositions.

## Properties you can rely on

- **Pure & stateless** — no instance state, no globals; inherently thread/fiber-safe and
  freely reusable. Same inputs always give the same result.
- **Symmetric** — `f(a, b) == f(b, a)`.
- **Boundary cases** — distance to self is `0`; distance to/from `""` is the other
  string's `count()`.
- **Space** — `O(min-axis)`: levenshtein keeps two rolling rows, damerau three (it needs
  row i-2 for the transposition term). Working rows are heap-allocated internally and
  freed before return — no caller cleanup.
- **Raises nothing** — there are no exceptions or sentinels; every input pair (including
  empty strings) yields a valid count.

## Example (mirrors test/search/DistanceTests.cpp)

```cajeta
import cajeta.search.distance.Distance;

int32 dam = Distance.damerau("flie", "file");        // 1  (one adjacent swap)
int32 lev = Distance.levenshtein("flie", "file");    // 2  (two substitutions)

int32 d   = Distance.levenshtein("kitten", "sitting"); // 3  (classic)
int32 s   = Distance.damerau("saturday", "sunday");    // 3, == damerau("sunday","saturday")
```
