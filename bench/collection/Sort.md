# Sort

`cajeta.collection.Sort` — general-purpose host sorting and the shared comparison protocol. The int64 row is a pattern matrix (random, ascending, descending, duplicates) after sort-research-rs; the first suite skipped three of the four because the quicksort of the day overflowed its range stack on sorted input.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/sort-int64` | sort 1e6 int64; variants random, ascending, descending, dups | C++ `std::sort` and pdqsort, Rust `sort_unstable` (ipnsort), Go `slices.Sort`, Java `Arrays.sort`, Python `sorted` | non-decreasing |
| `collection/sort-f64` | sort 1e6 random float64 | same | non-decreasing |
| `collection/sort-stable-int64` | stable sort of 1e6 (value, index) pairs | C++ `stable_sort`, Rust `sort` (driftsort), Go `SortStable`, Java `Arrays.sort` on objects, Python `sorted` | stable and non-decreasing |
| `collection/binary-search` | 1e7 binary searches in a sorted 1e6 array | C++ `lower_bound`, Rust `binary_search`, Go `sort.Search`, Java `Arrays.binarySearch`, Python bisect | all found |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/sort-int64
metric: time
```

```bench
id: collection/sort-f64
metric: time
```

```bench
id: collection/sort-stable-int64
metric: time
```

```bench
id: collection/binary-search
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench sort-int64               # Cajeta
./bench.sh --bench sort-int64 --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Sort](../../docs/stdlib/collection/Sort.md)
- Source: `runtime/src/cajeta/collection/Sort.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
