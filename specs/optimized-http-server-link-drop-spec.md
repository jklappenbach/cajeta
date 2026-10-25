# optimized-http-server-link-drop — optimized builds drop template drop functions that are still referenced; no cajeta-http server and not the tour links above O0 (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 19
> (cajeta-primavera bench), measured on v0.46.3. Blocks every release-build
> throughput number for cajeta-http and primavera.

## 1. Definition
A program that hosts a cajeta-http server fails to link at every optimized
setting tried: `--release` at O3 and at O1, and the debug base at O3. The
undefined symbols are instantiated drop functions:
`__cajeta_stack_cajeta_lang_Optional_dev_cajeta_http_server_Dispatch__drop`
and `…_primavera_web_Connection__drop`. An `Optional<T>` instantiated in the
bench's own module lost its drop the same way. Only O0 links. Unit 18 narrowed it: the drop function is a weak definition
in the stdlib module at O0, and the optimizer removes it while
`DispatchPool.worker` still calls it; `--link-mode=full`, `--tree-shake=off`,
the fork's lld on PATH, the Olla archive, a rebuilt checkout archive and
compiling the http sources in directly all fail the same way. The primavera
bench therefore builds its server on the debug base with the runtime checks
off and records that in `flags`; its register-user row reads about 4k req/s
against 60k to 130k for the JVM and .NET stacks, a number that says nothing
until the server is optimized. The DI app, which does not use the server,
builds at release O3 normally.

**Wider than http** (unit 15, 2026-10-25): `samples/tour` does not link at
`--release`, `--release --lto=off` or `--mode=debug-release` either, with the
same `undefined __cajeta_stack_*_drop` shape, so the toolchain benchmarks
build the tour with the debug flavor. Any optimized program that instantiates
a template whose drop lives in another module is at risk.

## 2. Use cases
- **2.1** As a developer, when I build a cajeta-http server program with
  `--release`, then it links: every template instantiation's drop function
  that an optimized module references is emitted, or the reference is
  removed with the dead instantiation.
- **2.2** As the test suite, then a JIT or AOT test instantiates
  `Optional<T>` over a class from a dependency archive inside an optimized
  build and links it; and cajeta-http's CI builds its tour server at
  `--release`.

## 3. Acceptance
`cajeta-primavera/bench/apps/web/FLAGS` reads `--release --opt=O3`, the
bench rebuilds, and the smoke passes; the same for `cajeta-http/bench`.
