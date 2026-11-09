# Loops

This page measures the language's loops bare. The rows are a counted `for`, the same loop with bounds checks off, nested loops, `foreach` over an array and over an `ArrayList`, a range, a loop-carried reduction and an early exit. These are the floors every other number on this site stands on. Each loop consumes its result through the harness sink so the optimizer cannot delete it, and the smoke test greps the release IR to confirm the loop survived.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/loop-counted` | 1e8 iterations summing `i` into an int64 | C++, Rust, Go, Java, Python | sum |
| `lang/loop-counted-nobounds` | the same loop indexing an array with `--bounds=off` | C++, Rust (`get_unchecked`), Go, Java | sum |
| `lang/loop-nested` | 1e4 × 1e4 nested counted loops | C++, Rust, Go, Java, Python | sum |
| `lang/loop-foreach-array` | `foreach` over an `int32[]` of 1e7 | C++, Rust, Go, Java, Python | sum |
| `lang/loop-foreach-arraylist` | walk an `ArrayList<int32>` of 1e7 by index (`foreach` over an `ArrayList` runs zero iterations on v0.46.3, a filed defect; the row moves to `foreach` when fixed) | C++, Rust, Go, Java, Python | sum |
| `lang/loop-range` | a counted `for` over [lo, hi) of 1e8 (the language has no range construct; the competitors use theirs) | C++, Rust, Go, Java, Python | sum |
| `lang/loop-reduce` | loop-carried float64 reduction over 1e7 | C++, Rust, Go, Java, Python | sum within epsilon |
| `lang/loop-early-exit` | scan of 1e7 for a sentinel at the midpoint, 1e3 times | C++, Rust, Go, Java, Python | index found |

The language has no loop construct a competitor lacks, so every row is the same shape in every language.

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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

## Standing

Counted loops, nested loops, ranges, reductions and both foreach forms are at parity with Rust and C++ (0.96x to 1.21x), with Go and Java 3x to 9x slower than Cajeta on the counted forms. Without bounds checks the indexed loop is 1.02x of Rust's get_unchecked; the bounds-on variant has no competitor pair. Early exit is 5.9x behind C++ and Rust and 2.4x behind Java. Not yet investigated.

## Reproduce

```sh
cd bench
./bench.sh --bench loop-counted               # Cajeta
./bench.sh --bench loop-counted --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/lang/`
