# cajeta.search — Specification

> Status: **draft, pending approval.** Authored with the **design** skill. A cajeta
> **standard-library** package (target-language code under `runtime/src/cajeta/search/`),
> distinct from the build-tool's host-side C++ skill matcher (`src/cajeta/buildtool/skill/`),
> which stays as the compiler's build-time engine. The two are parallel implementations at
> two layers (host C++ vs. target cajeta); they share **behavior** (and test vectors), not
> code. Branch: `feature/mcd-lift`.

## 1. Definition

### 1.1 Purpose
A standard-library package giving cajeta **programs** approximate ("fuzzy") text matching:
look up values by a key string that may be misspelled. The first and primary module is an
**n-gram inverted index** (`cajeta.search.ngram`) for sub-linear candidate generation;
sibling modules supply string-distance scoring and a matcher that composes the two.

### 1.2 Why
cajeta programs that offer command palettes, autocomplete, symbol/file finders, "did you
mean", or near-duplicate detection all need the same primitive: given a query, cheaply find
the stored keys *close* to it. The build tool already proved out this design in host C++
(skill discovery, `docs/specs/skill-discovery-spec.md` §3.5); this package brings the same
capability to cajeta code. It is an **index/retrieval** family, not a collection — it is
*built over* collections (`HashMap`, `ArrayList`, `HashSet`) but its contract is retrieval
(`candidates(query)`), not containment.

### 1.3 Scope & package layout
- `cajeta.search.ngram` — the n-gram inverted index (this spec's core; built first).
- `cajeta.search.distance` — string-distance functions (Levenshtein, Damerau/OSA).
- `cajeta.search.fuzzy` — a matcher composing the index (prefilter) + distance (scoring) +
  ranking.

Files live at `runtime/src/cajeta/search/<Type>.cajeta` with `package cajeta.search.ngram;`
etc., mirroring `cajeta.collection` conventions (PascalCase types, camelCase methods,
`<T>`/`<K,V>` generics, JavaDoc-style docs, `heap`/ownership `#`).

### 1.4 Non-goals
- Full-text / token inverted indexing with relevance ranking (TF-IDF/BM25) — a future
  `cajeta.search.text` sibling, not this package.
- On-disk / persistent indexes; query-language parsing.
- Semantic / embedding search (matching by meaning). Spelling-tolerant matching is in scope.
- Unicode-collation-correct distance; v1 operates on the string's existing
  codepoint/char surface (good enough for identifier- and title-style keys).

## 2. `cajeta.search.ngram` — the index

### 2.1 Requirements
- A generic `Index<T>` mapping **key strings** to stored **values** of type `T`, built over
  the existing collections (an `Index<T>` is *not* itself a collection — §1.2).
- Construction selects the gram size `n` (default 3 = trigrams). Gram extraction is
  case-folded (lowercased) for recall.
- `add(key, value)` indexes `value` under every distinct n-gram of `key`. A key shorter than
  `n` is indexed under itself as a single gram (so short keys still match).
- `candidates(query)` returns every value whose key shares **at least one** n-gram with the
  query, **deduplicated** (a value indexed under many shared grams appears once). This is a
  recall-oriented prefilter: a superset the caller (or `cajeta.search.fuzzy`) then scores.
- A query sharing no gram with any key returns an empty result (the index actually filters).
- Sub-linear in the indexed key count: `candidates` visits only the postings of the query's
  grams, never a full scan.

### 2.2 Use cases
- **2.2.1** As a cajeta program, I `add("cajeta/io/File", fileEntry)` then
  `candidates("cajeta/io/Fiel")` and `fileEntry` is in the result (recall under a typo).
- **2.2.2** As a program, a value indexed under a key with repeated grams is returned **once**
  from `candidates`.
- **2.2.3** As a program, `candidates("zzqqww")` against unrelated keys is empty.
- **2.2.4** As a program, a key shorter than `n` (e.g. `"io"`, trigrams) is still findable by
  an exact or near query.
- **2.2.5** As a program, I can construct `Index<T>(n)` with a different gram size (e.g.
  bigrams) and indexing/lookup use that `n`.

## 3. `cajeta.search.distance` — scoring

### 3.1 Requirements
- `levenshtein(a, b): int32` — classic edit distance (insert/delete/substitute).
- `damerau(a, b): int32` — optimal string alignment (adjacent transpositions cost 1), so
  `damerau("flie","file") == 1` while `levenshtein` is 2.
- Pure functions, no allocation beyond working buffers; symmetric in their arguments.

### 3.2 Use cases
- **3.2.1** As a program, `damerau("flie", "file")` returns 1 (transposition-aware).
- **3.2.2** As a program, `levenshtein("kitten", "sitting")` returns 3.
- **3.2.3** As a program, distance to an identical string is 0; to the empty string is the
  other's length.

## 4. `cajeta.search.fuzzy` — the matcher

### 4.1 Requirements
- `Matcher<T>` composes an `ngram.Index<T>` (prefilter) with a distance function (scoring)
  and a length-scaled threshold, returning **ranked** results closest-first.
- `add(key, value)` feeds both the index and the key store (the matcher must remember each
  value's key to score it). `match(query): #ArrayList<Match<T>>` where `Match<T>` carries the
  value, the matched key, and the distance; ordered by ascending distance then key.
- A threshold rejects matches beyond a length-scaled bound (no garbage). An `exact` query
  path returns only distance-0 matches.

### 4.2 Use cases
- **4.2.1** As a program, after adding several keys, `match("Fiel")` ranks the `"File"` entry
  first with distance 1, and omits unrelated entries beyond threshold.
- **4.2.2** As a program, identical queries produce identical ranked output (deterministic).
- **4.2.3** As a program, an exact match outranks a fuzzy one.

## 5. Testing
Standard-library types are tested via the JIT gtest harness (`CajetaJit::compile(src, ...)`
in `test/`): each test compiles a small cajeta program that imports the package, exercises
it, and returns an `int32` the C++ side asserts on (model: `test/collections/NewCollectionsTests.cpp`).
Behavior parity with the host C++ skill matcher is maintained by sharing the same query →
expected-result vectors across both implementations (skill-discovery spec §3.5).

## 6. Relationship to skill discovery
This package does **not** replace the build-tool's C++ `SkillIndex`/`SkillMatcher`: the
toolchain runs at build time and cannot call target stdlib. The duplication is deliberate and
small; it has a natural exit if cajeta ever self-hosts. `cajeta.search` may itself ship a
skill (the system documenting itself).
