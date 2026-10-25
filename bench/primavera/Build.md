# Build time

`primavera` — a clean build of the register-user service.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `primavera/build-time` | clean build to a runnable artifact | Maven for Spring Boot, Quarkus and Micronaut (JVM; native-image as a second row), `dotnet publish`, none for FastAPI | artifact runs |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: primavera/build-time
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench build-time               # Cajeta
./bench.sh --bench build-time --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/primavera/`
- Library: `cajeta-primavera`
