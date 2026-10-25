# Class

`cajeta.reflect.Class` — runtime type information.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `reflect/class-lookup` | 1e7 `Class` lookups by name | Java `Class.forName` (cached), Go `reflect.TypeOf` (no by-name lookup), Python `getattr(module, name)` | count |
| `reflect/field-enumerate` | enumerate the fields of a 20-field class 1e6 times (exported names, so Go can see them) | Java reflection, Go reflect, Python `vars` | field count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: reflect/class-lookup
metric: time
```

```bench
id: reflect/field-enumerate
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench class-lookup               # Cajeta
./bench.sh --bench class-lookup --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Class](../../docs/stdlib/reflect/Class.md)
- Source: `runtime/src/cajeta/reflect/Class.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/reflect/`
