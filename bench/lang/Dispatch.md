# Calls and dispatch

A direct call, a virtual call through a base class, an interface call, a call through a monomorphized template, a lambda, a method reference and `instanceof`. Cajeta templates are instantiated per type the way C++ templates are, so the template row is the one to read against Java's erased generics.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/call-direct` | 1e8 calls to a static method the optimizer may not inline | C++, Rust, Go, Java, Python | sum |
| `lang/call-virtual` | 1e8 calls through a base-class reference over two subclasses | C++, Rust, Go, Java, Python | sum |
| `lang/call-interface` | 1e8 calls through an interface reference | C++ (abstract class), Rust `dyn Trait`, Go interface, Java, Python | sum |
| `lang/call-template` | 1e8 calls to a `T`-parameterized method instantiated at `int32` | C++ template, Rust generic fn, Go generic, Java generic (boxed), Python | sum |
| `lang/call-lambda` | 1e8 lambda invocations capturing one local | C++, Rust, Go, Java, Python | sum |
| `lang/call-method-ref` | 1e8 calls through a method reference | C++ `std::function`, Rust fn pointer, Go func value, Java, Python | sum |
| `lang/instanceof` | 1e8 `instanceof` checks over a three-level hierarchy | C++ `dynamic_cast`, Rust `Any`, Go type switch, Java, Python `isinstance` | count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/call-direct
metric: time
```

```bench
id: lang/call-virtual
metric: time
```

```bench
id: lang/call-interface
metric: time
```

```bench
id: lang/call-template
metric: time
```

```bench
id: lang/call-lambda
metric: time
```

```bench
id: lang/call-method-ref
metric: time
```

```bench
id: lang/instanceof
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench call-direct               # Cajeta
./bench.sh --bench call-direct --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/lang/`
