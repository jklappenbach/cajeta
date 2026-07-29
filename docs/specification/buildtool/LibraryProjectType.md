# Library Project Type — Specification

_Status: **shipped** (archetype registered in the build tool, builds verified to a
`.cja`). Spec covers the library project type, the `cajeta init library` archetype, and
how it differs from a binary project. See `plans/library-archetype-plan.md` for rollout
and follow-ups._

## 1. Purpose

A **library project** compiles a set of Cajeta packages into a reusable **`.cja`
archive** that other projects depend on, rather than a native executable. It is the
counterpart to the `basic` (binary) archetype. The defining property: a library has
**no `main` / no entry point**.

Use a library project for: shared APIs, framework code (e.g. `cajeta-logging`,
`cajeta-unit`), and any code meant to be consumed by other Cajeta projects via
`settings.dependencies`.

## 2. The one structural difference: no entry point

A binary and a library project are identical except for a single manifest field:

| | Binary (`basic`) | Library (`library`) |
|---|---|---|
| `settings.build.entry-method` | present (`pkg.Main::main`) | **absent** |
| `build` action emits | native executable | **`.cja` archive** |
| Has a `main` | yes | **no** |
| Output path | `build/exe/<pkg>/…` or task `output-path` | `build/archive/<name>-<version>.cja` |

With no entry point the compiler has nothing to link into an executable, so the `build`
action produces a library archive instead. There is no separate `--emit=lib` flag at the
project level — the **presence or absence of `entry-method` is the signal.**

## 3. `cajeta init library [<dir>]`

Writes the embedded `library` archetype to `<dir>` (default `.`):

```
cajeta.json                                          # library manifest (no entry-method)
src/main/cajeta/com/example/library/Greeter.cajeta   # sample public API — NO main
```

Only `cajeta.json` and `src/**/*.cajeta` are part of the archetype (the embedder globs
exactly those; a `README.md` in the template tree is documentation, not scaffolded).

The archetype is registered alongside `basic`, `workspace`, `multi-binary`, and `melt`
(`cajeta init --list`), embedded at compiler-build time from
`samples/buildtool/library/` via `cmake/EmbedInitTemplates.cmake`
(`src/CMakeLists.txt: CAJETA_INIT_TEMPLATE_NAMES`).

## 4. Manifest specification

The library manifest (`samples/buildtool/library/cajeta.json`) is the canonical shape:

- **`details`** — `name` (reverse-DNS, e.g. `org.acme.json`), `version` (semver),
  `description`, `license`, `authors`, `cajeta-lang-version`. `name` is how repositories
  address the published artifact and must match the source package namespace.
- **`settings.capabilities`** — declare only what the library's API actually needs;
  default `[]` for a pure-computation library. Capabilities are **re-validated against
  the consuming project's manifest** at the point of use — a library cannot silently
  grant itself `network`/`filesystem`; the consumer must also declare them.
- **`settings.dependencies`** — third-party runtime deps. The core stdlib is built into
  the toolchain (neither declared nor fetched); dead-code elimination links only the
  parts a consumer uses.
- **`settings.dev-dependencies`** — test-only deps (e.g. `cajeta.testkit`); **not**
  shipped in the published `.cja`.
- **`settings.build`** — `target` only. **No `entry-method`** (the library signal).
- **`tasks`** — `build` (→ `.cja`), `test`, `release` (optimized `.cja`), `clean`.

## 5. Build outputs

- `cajeta build` → `build/archive/<details.name>-<details.version>.cja`, plus a
  `sha256:` digest in the task `outputs` (reproducible-build identity).
- `cajeta release` → the same path, release flavor (optimized).
- The `.cja` bundles the compiled packages + the manifest; consumers fetch it from a
  configured repository (`settings.repositories`) and link against it.

## 6. Consuming a library

A downstream project depends on a published library by name + version range under its
own `settings.dependencies`:

```json
"dependencies": { "org.acme.json": "1.2.*" }
```

The build tool resolves the version, fetches the `.cja` from the highest-priority
repository that has it, verifies its `sha256`, and puts it on the compiler `--classpath`.
The library's declared capabilities are unioned into the consumer's required-capability set
and checked against what the consumer's manifest permits.

**How a dep's code reaches the binary.** A classpath `.cja` always contributes its class
*declarations* (so the consumer's source type-checks against the library API). For
`--emit=exe`/`--emit=obj`, the consumer additionally re-drives each classpath class's body
through its *own* codegen (from the source bytes the `.cja` bundles) and emits those objects
into the link — so the library's code is actually linked, target-correct, and pulls in
exactly the stdlib template instantiations it needs (a published library `.cja` is
stdlib-stripped, so its own bitcode entries can't be linked standalone). `--gc-sections`
then drops whatever the entry point never reaches. `--emit=cja`/`--emit=uber` keep deps
external: a `cja` ships declarations only; an `uber` bundles each dep's bitcode under
`deps/<name>-<version>/` for a self-contained, JIT-hosted artifact.

## 7. Conventions

- **Package = name.** Source packages live under the reverse-DNS namespace in
  `details.name` (rename the scaffolded `com.example.library` to match).
- **No `main`, ever.** If a library grows an executable surface (a CLI), that belongs in
  a separate binary project that depends on the library — keep the library importable.
- **Test sources** live under a test root and use `cajeta.testkit`; they are built for
  the `test` task only and excluded from the published archive.

## 8. Out of scope / future (tracked in the plan)

- A richer archetype that also scaffolds a `test/` tree and a sample test.
- A one-line description per archetype in `cajeta init --list`.
- An archetype regression test asserting `init library` → `build` → `.cja`.
- Multi-package and workspace-member library layouts (compose with the `workspace`
  archetype).
