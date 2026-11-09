# Slice

`cajeta.lang.Slice` is the zero-copy array window behind `arr[a:b]`. Creating one must not copy, and iterating one must cost what iterating the array costs.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/slice-window` | 1e7 slices of 64 elements taken from a 1e6 array | C++ `std::span`, Rust `&a[a..b]`, Go `a[a:b]`, Java `Arrays.copyOfRange` (copies; noted), Python `memoryview` | sum of first elements |
| `lang/slice-iterate` | walk a 1e7-element slice by index 10 times (`foreach` over a `Slice` runs zero iterations today, same defect) | same | sum |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/slice-window
metric: time
```

```bench
id: lang/slice-iterate
metric: time
```

## Standing

A 64-element window is 14x behind std::span and 10x behind Rust and Go slices; iterating a slice 9x behind span and 7x behind Rust. Java's copyOfRange, which copies, is 1.4x slower than Cajeta. The page's contract is that a slice costs nothing to take and nothing extra to walk, so both rows are defects; not yet scheduled.

## Reproduce

```sh
cd bench
./bench.sh --bench slice-window               # Cajeta
./bench.sh --bench slice-window --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Slice](../../docs/stdlib/lang/Slice.md)
- Source: `runtime/src/cajeta/lang/Slice.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
