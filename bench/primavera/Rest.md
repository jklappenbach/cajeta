# REST endpoint

`primavera` — the `register-user` shape: a JSON `POST /users` through dependency injection, a request-scoped principal from an authentication stage, a singleton service, and validation, answered with JSON. Nothing is stored (there is no data layer yet; the id is minted from the body), so the same request can be sent under load. Every competitor implements the same contract and is byte-compared before load. Until the optimized-build link defect is fixed, primavera's server row is built at O0 and its `flags` column says so.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `primavera/register-user-json` | `POST /users` with a validated body behind an auth filter; variants 64, 512 connections | Spring Boot (MVC, Java 25), Quarkus (JVM and native), Micronaut (JVM and native), ASP.NET Core, FastAPI | zero errors, body equal |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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

- Benchmark source: `cajeta-primavera/bench/`
- Library: `cajeta-primavera`, benchmark code in `cajeta-primavera/bench/`
- Sample: `cajeta-primavera/samples/register-user`
