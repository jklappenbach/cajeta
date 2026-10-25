# REST endpoint

`primavera` — the `register-user` shape: a JSON request through dependency injection, request scope, validation and an authentication filter, answered with JSON. This is the request a service framework exists to serve; every competitor implements the same contract and is byte-compared before load.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `primavera/register-user-json` | `POST /users` with a validated body behind an auth filter; variants 64, 512 connections | Spring Boot (MVC, Java 25), Quarkus (JVM and native), Micronaut (JVM and native), ASP.NET Core, FastAPI | zero errors, body equal |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: primavera/register-user-json
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench register-user-json               # Cajeta
./bench.sh --bench register-user-json --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/primavera/`
- Library: `cajeta-primavera`, benchmark code in `cajeta-primavera/bench/`
- Sample: `cajeta-primavera/samples/register-user`
