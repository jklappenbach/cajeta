# Loops

The language's loops, measured bare: a counted `for`, the same loop with bounds checks off, nested loops, `foreach` over an array and over an `ArrayList`, a range, a loop-carried reduction and an early exit. These are the floors every other number on this site stands on. Each loop consumes its result through the harness sink so the optimizer cannot delete it, and the smoke test greps the release IR to confirm the loop survived.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/loop-counted` | 1e8 iterations summing `i` into an int64 | C++, Rust, Go, Java, Python | sum |
| `lang/loop-counted-nobounds` | the same loop indexing an array with `--bounds=off` | C++, Rust (`get_unchecked`), Go, Java | sum |
| `lang/loop-nested` | 1e4 × 1e4 nested counted loops | C++, Rust, Go, Java, Python | sum |
| `lang/loop-foreach-array` | `foreach` over an `int32[]` of 1e7 | C++, Rust, Go, Java, Python | sum |
| `lang/loop-foreach-arraylist` | `foreach` over an `ArrayList<int32>` of 1e7 | C++, Rust, Go, Java, Python | sum |
| `lang/loop-range` | range iteration over 1e8 | C++, Rust, Go, Java, Python | sum |
| `lang/loop-reduce` | loop-carried float64 reduction over 1e7 | C++, Rust, Go, Java, Python | sum within epsilon |
| `lang/loop-early-exit` | scan of 1e7 for a sentinel at the midpoint, 1e3 times | C++, Rust, Go, Java, Python | index found |

The language has no loop construct a competitor lacks, so every row is the same shape in every language.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/loop-counted
metric: time
```

```bench
id: lang/loop-counted-nobounds
metric: time
```

```bench
id: lang/loop-nested
metric: time
```

```bench
id: lang/loop-foreach-array
metric: time
```

```bench
id: lang/loop-foreach-arraylist
metric: time
```

```bench
id: lang/loop-range
metric: time
```

```bench
id: lang/loop-reduce
metric: time
```

```bench
id: lang/loop-early-exit
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench loop-counted               # Cajeta
./bench.sh --bench loop-counted --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/lang/`
