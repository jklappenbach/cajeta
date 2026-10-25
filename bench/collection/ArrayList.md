# ArrayList

`cajeta.collection.ArrayList` — the growable, index-addressable sequence backed by a doubling `T[]`. Append is the row that mattered in the first suite (5× behind `Vec`); get, iterate and sort complete the picture.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `collection/arraylist-append` | append 1e7 int32 to an empty list | C++ `vector`, Rust `Vec`, Go slice, Java `ArrayList<Integer>` (boxed; noted), Python list | count |
| `collection/arraylist-get` | 1e8 random `get` on a 1e6 list | same | sum |
| `collection/arraylist-iterate` | `foreach` over 1e7 elements, 10 times | same | sum |
| `collection/arraylist-sort` | `sort()` of 1e6 random int32 | same libraries' sorts | non-decreasing |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: collection/arraylist-append
metric: time
```

```bench
id: collection/arraylist-get
metric: time
```

```bench
id: collection/arraylist-iterate
metric: time
```

```bench
id: collection/arraylist-sort
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench arraylist-append               # Cajeta
./bench.sh --bench arraylist-append --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [ArrayList](../../docs/stdlib/collection/ArrayList.md)
- Source: `runtime/src/cajeta/collection/ArrayList.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/collection/`
