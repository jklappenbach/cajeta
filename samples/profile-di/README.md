# profile-di — DI profiles & test doubles

Shows how the compile-time DI substrate (`cajeta.aot`) selects components by
build profile, with no code change — just a different `--profile=<name>` at
compile time. One source, three builds:

| `--profile=` | `Store`  | `Greeting`      | why |
|--------------|----------|-----------------|-----|
| `prod`       | `sql`    | `real-greeting` | `SqlStore @Profile("prod")`; `ProdGreeting` neutral |
| `dev`        | `mem`    | `real-greeting` | `MemStore @Profile({"dev","test"})` (any-of) |
| `test`       | `mem`    | `fake-greeting` | `MemStore` (via any-of); `@TestComponent FakeGreeting` masks the neutral `ProdGreeting` by shared interface |

Three features in one demo:
- **Profile selection** — `@Profile("prod")` vs `@Profile({"dev","test"})` on `Store`.
- **Any-of** — `MemStore` is eligible under both `dev` and `test`.
- **Test-double masking** — under `--profile=test`, `@TestComponent FakeGreeting`
  masks the profile-neutral `@Component ProdGreeting` because both implement
  `Greeting`. Masking is interface-scoped; a double with no interface masks nothing.

## Run

```
./run.sh
```

Builds the source under each profile (`--emit=exe`), runs each binary, and checks
the resolved `Store`/`Greeting`. Exits nonzero on any mismatch, so it doubles as
the demo's test. Override the compiler with `CAJETA_BIN=/path/to/cajeta`.

See `docs/specification/lang/AspectModel.md` § "Profiles and test-component
masking" for the full semantics.
