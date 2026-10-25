# Matcher

`cajeta.search.fuzzy.Matcher` — typo-tolerant key lookup over an n-gram prefilter.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `search/fuzzy-topk` | top-10 matches for 1e4 queries over 1e5 keys; the check is recall@10 ≥ 0.95 against OSA brute force (the bigram prefilter misses some keys; `Matcher` has no top-k limit and ranks the full list) | Rust `nucleo-matcher` (fzf-style score, checked against its own brute force), Python `rapidfuzz.process`, Java Lucene `FuzzyQuery` (edits capped at 2) | recall against brute force |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: search/fuzzy-topk
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench fuzzy-topk               # Cajeta
./bench.sh --bench fuzzy-topk --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Matcher](../../../docs/stdlib/search/fuzzy/Matcher.md)
- Source: `runtime/src/cajeta/search/fuzzy/Matcher.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/search/`
