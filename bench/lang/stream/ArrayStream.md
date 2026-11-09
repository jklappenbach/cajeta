# ArrayStream

`cajeta.lang.stream.ArrayStream` — a `Stream` over a contiguous `T[]` range, the stream every array and `ArrayList` hands out.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/arraystream-iterate` | pull every element of a 1e7 array through `stream()`, 10 times | C++ ranges over a vector, Rust `iter()`, Go range, Java `Arrays.stream`, Python iteration | sum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/arraystream-iterate
metric: time
```

## Standing

Map and sum over an array is 81x behind Rust's iterator and 25x to 100x behind C++ ranges, a Go loop and Java's Arrays.stream. The same loss as the [Stream](Stream.md) rows; not yet investigated.

## Reproduce

```sh
cd bench
./bench.sh --bench arraystream-iterate               # Cajeta
./bench.sh --bench arraystream-iterate --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [ArrayStream](../../../docs/stdlib/lang/stream/ArrayStream.md)
- Source: `runtime/src/cajeta/lang/stream/ArrayStream.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
