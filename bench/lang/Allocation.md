# Allocation and ownership

The cost of the single-owner heap: allocating a small object with `heap` and dropping it at scope end, churn with one and ten objects live, transferring ownership with `#` down a chain of calls, the live-allocation set on and off, arrays, and struct values that never touch the heap. The memory tab of every chart shows allocated bytes, and for the live-set rows the chart shows the same program under `--live-set=on` and `--live-set=off`.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/alloc-small-object` | 1e7 `heap` allocations of a 32-byte object, each dropped at scope end | C++ `new`/`delete` and `make_unique`, Rust `Box`, Go, Java, Python | count |
| `lang/alloc-churn-1` | 1e7 allocations with one object live at a time | same | count |
| `lang/alloc-churn-10` | 1e7 allocations with ten live, oldest dropped | same | count |
| `lang/alloc-transfer-chain` | 1e6 objects passed with `#` through ten calls and dropped at the end | C++ `std::move` of `unique_ptr`, Rust move, Go, Java, Python | count |
| `lang/alloc-live-set-on` | `alloc-small-object` with the live-allocation set on | Cajeta only (the two compiler modes) | count |
| `lang/alloc-live-set-off` | `alloc-small-object` with the live-allocation set off | Cajeta only | count |
| `lang/alloc-array` | 1e5 allocations of an `int32[1024]` | C++, Rust, Go, Java, Python | sum of first elements |
| `lang/alloc-struct-value` | 1e8 struct values constructed and read on the stack | C++, Rust, Go, Java record, Python | sum |

The ownership model is the load-bearing design of the language; read the [memory model](../../docs/specification/lang/MemoryModel.md) before reading these numbers. Java and Go rows include their collectors' work in the measured window; the allocation-bytes metric is the one that compares runtimes on equal terms.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/alloc-small-object
metric: time
```

```bench
id: lang/alloc-churn-1
metric: time
```

```bench
id: lang/alloc-churn-10
metric: time
```

```bench
id: lang/alloc-transfer-chain
metric: time
```

```bench
id: lang/alloc-live-set-on
metric: time
```

```bench
id: lang/alloc-live-set-off
metric: time
```

```bench
id: lang/alloc-array
metric: time
```

```bench
id: lang/alloc-struct-value
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench alloc-small-object               # Cajeta
./bench.sh --bench alloc-small-object --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/lang/`
