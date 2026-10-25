# StringBuilder

`cajeta.lang.StringBuilder` — the growable byte buffer that builds a `String` in amortized O(N). It is the answer to `string-build-concat`, and the two charts belong side by side.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/stringbuilder-append` | append a 16-byte piece 1e6 times, `toString` once | C++ `std::string::append`, Rust `String::push_str`, Go `strings.Builder`, Java `StringBuilder`, Python `''.join` | length |
| `lang/stringbuilder-append-mixed` | append int32, float64 and String pieces in rotation, 1e6 times | same | length |
| `lang/stringbuilder-tostring` | `toString` of a 16 MB builder, 1e2 times | same | length |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/stringbuilder-append
metric: time
```

```bench
id: lang/stringbuilder-append-mixed
metric: time
```

```bench
id: lang/stringbuilder-tostring
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench stringbuilder-append               # Cajeta
./bench.sh --bench stringbuilder-append --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [StringBuilder](../../docs/stdlib/lang/StringBuilder.md)
- Source: `runtime/src/cajeta/lang/StringBuilder.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
