# Start-up and footprint

`primavera` — cold start to the first response, and resident memory idle and under load. Compile-time injection should show here first.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `primavera/cold-start` | `exec` to first 200 response, 20 launches | Spring Boot (MVC, Java 25), Quarkus (JVM and native), Micronaut (JVM and native), ASP.NET Core, FastAPI | response |
| `primavera/idle-rss` | resident set after start-up | Spring Boot (MVC, Java 25), Quarkus (JVM and native), Micronaut (JVM and native), ASP.NET Core, FastAPI | alive |
| `primavera/loaded-rss` | resident set after 60 s at 512 connections | Spring Boot (MVC, Java 25), Quarkus (JVM and native), Micronaut (JVM and native), ASP.NET Core, FastAPI | alive |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: primavera/cold-start
metric: time
```

```bench
id: primavera/idle-rss
metric: memory
```

```bench
id: primavera/loaded-rss
metric: memory
```

## Reproduce

```sh
cd bench
./bench.sh --bench cold-start               # Cajeta
./bench.sh --bench cold-start --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/primavera/`
- Library: `cajeta-primavera`
