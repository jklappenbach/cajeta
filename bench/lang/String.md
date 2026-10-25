# String

`cajeta.lang.String` — immutable UTF-8 text, the single string type. Four rows date from the first suite and are kept as they were; the new rows walk codepoints, scan bytes, split and compare, which is where a UTF-8 string with byte-offset indexing differs from a UTF-16 one.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/string-build-concat` | build 32 KB by repeated `+` | C++ `std::string`, Rust `String`, Go, Java, Python | length |
| `lang/string-search` | `indexOf` of a 12-byte needle in a 1 MB haystack, 1e3 times | C++ `find`, Rust `find`, Go `strings.Index`, Java `indexOf`, Python `find` | index |
| `lang/string-replace` | replace every occurrence of a word in 1 MB | same libraries | length |
| `lang/string-uppercase` | `toUpperCase` of 1 MB ASCII and 1 MB mixed-script text | same libraries | checksum |
| `lang/string-codepoint-walk` | iterate every codepoint of 1 MB mixed-script text | C++ hand-decoded, Rust `chars()`, Go `range`, Java `codePoints()`, Python `for c in s` | count |
| `lang/string-byteat-scan` | `byteAt` over 1 MB counting newlines | C++ `operator[]`, Rust `bytes()`, Go `[]byte`, Java `getBytes` once then index, Python `bytes` | count |
| `lang/string-split` | split 1 MB on commas, 1e2 times | same libraries | piece count |
| `lang/string-compare` | 1e7 comparisons of 32-byte strings, half equal | same libraries | equal count |

`count()` is codepoints; `indexOf`, `substring` and `byteAt` take byte offsets. The rows name which they use.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/string-build-concat
metric: time
```

```bench
id: lang/string-search
metric: time
```

```bench
id: lang/string-replace
metric: time
```

```bench
id: lang/string-uppercase
metric: time
```

```bench
id: lang/string-codepoint-walk
metric: time
```

```bench
id: lang/string-byteat-scan
metric: time
```

```bench
id: lang/string-split
metric: time
```

```bench
id: lang/string-compare
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench string-build-concat               # Cajeta
./bench.sh --bench string-build-concat --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [String](../../docs/stdlib/lang/String.md)
- Source: `runtime/src/cajeta/lang/String.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
