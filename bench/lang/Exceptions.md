# Exceptions

What a `try` costs when nothing is thrown, what a throw costs when it is caught one frame up, and how that scales through ten and a hundred frames. `RecoverableException` and `UnrecoverableException` are measured separately, and one row throws inside a fiber. Rust and Go have no exceptions, so their rows propagate a `Result` or an `error` through the same call depth and say so in the row's notes.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/try-no-throw` | 1e8 calls through a `try` that never throws | C++, Java, Python; Rust and Go: a `Result`/`error` return | sum |
| `lang/throw-catch-1` | 1e6 throw-and-catch across one frame | C++, Java (with and without stack trace), Python; Rust `Err`, Go `error` | count caught |
| `lang/throw-catch-10` | 1e6 throws unwinding ten frames | same | count caught |
| `lang/throw-catch-100` | 1e5 throws unwinding a hundred frames | same | count caught |
| `lang/throw-recoverable` | `RecoverableException` against `UnrecoverableException`, 1e6 each | Java checked and unchecked | count caught |
| `lang/throw-in-fiber` | 1e5 throws caught inside a spawned fiber | Java virtual thread, Go goroutine, Python asyncio task | count caught |

The error model is documented in the guide's [error handling](../../docs/guide/20-error-handling.md) chapter.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/try-no-throw
metric: time
```

```bench
id: lang/throw-catch-1
metric: time
```

```bench
id: lang/throw-catch-10
metric: time
```

```bench
id: lang/throw-catch-100
metric: time
```

```bench
id: lang/throw-recoverable
metric: time
```

```bench
id: lang/throw-in-fiber
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench try-no-throw               # Cajeta
./bench.sh --bench try-no-throw --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/lang/`
