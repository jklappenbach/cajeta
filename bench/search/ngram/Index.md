# Index

`cajeta.search.ngram.Index` — the n-gram inverted index.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `search/ngram-build` | index 1e5 keys | Rust, Python, Java Lucene | posting count |
| `search/ngram-query` | 1e5 candidate queries | same | candidate count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: search/ngram-build
metric: time
```

```bench
id: search/ngram-query
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench ngram-build               # Cajeta
./bench.sh --bench ngram-build --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Index](../../../docs/stdlib/search/ngram/Index.md)
- Source: `runtime/src/cajeta/search/ngram/Index.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/search/`
