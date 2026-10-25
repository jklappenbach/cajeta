# Exemptions

A stdlib page listed here has no benchmark page, and `test/coverage_smoke.sh`
accepts that. Each line names the page and the reason. A reason is one of:
*pure type* (a value with no hot path), *protocol* (an interface measured
through its implementations), *exception* (measured under `lang/Exceptions`),
*windowed* (needs a display; not measured), *network* (depends on a remote
host), *session* (package management; not a runtime path).

| Page | Reason |
|---|---|
| `aot/Scope` | pure type; the graph cost is under `aot/Components` |
| `aot/Scoped` | pure type; same |
| `error/Exception` | exception |
| `error/NoOptionalValueException` | exception |
| `error/NotProvidedException` | exception |
| `error/RecoverableException` | exception; its throw cost is `lang/throw-recoverable` |
| `error/ScopeNotActiveException` | exception |
| `error/ScopedBuildFailedException` | exception |
| `error/Throwable` | exception |
| `error/UnrecoverableException` | exception; its throw cost is `lang/throw-recoverable` |
| `gfx/Sampler` | pure type; sampling cost is `gfx/texture-sample-rate` |
| `ifx/BackendRegistry` | windowed |
| `ifx/Window` | windowed |
| `io/net/IpAddress` | pure type |
| `io/net/ServerBuilder` | pure type; the server is measured under `io/net/Server` |
| `io/net/SocketAddress` | pure type |
| `io/net/dns/Dns` | network |
| `math/Camera` | pure type over `Transform` |
| `math/Color` | pure type |
| `math/DType` | pure type |
| `session/PackageInstallException` | exception |
| `session/Packages` | session |
| `time/Period` | pure type; calendar arithmetic is `time/time-localdate-arith` |
| `time/ZoneId` | pure type; zone application is `time/zoneddatetime-region` |
| `time/ZoneOffset` | pure type |
| `wire/Compressor` | protocol; measured through `wire/CompressStream` |
| `wire/Decompressor` | protocol; same |
| `wire/DecompressionLimitException` | exception |
| `wire/Schema` | pure type; measured through `wire/SchemaEncoder` |
