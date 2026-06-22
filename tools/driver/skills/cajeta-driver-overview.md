---
id: cajeta-driver-overview
applies-to: [cajeta-driver]
title: cajeta front-end binary — dispatch, config precedence, command routing
description: What the `cajeta` umbrella binary is and how to route a task to the right subcommand (archive / build-tool / jit-run / doc / ide / version) before it falls through to a raw compile.
---

# cajeta driver (the `cajeta` front-end binary)

`cajeta` is the single umbrella binary. Its `main` (`src/main.cpp`, **not**
`tools/driver/Driver.cpp` — that file is the legacy tinylang driver) inspects
`argv[1]`, short-circuits into a subcommand surface if it recognizes one, and
**otherwise falls through to the raw compiler** (`Compiler::compile`). So
`cajeta` is "subcommand-or-compile": if the first arg isn't a known verb, the
whole command line is parsed as compiler flags + three positionals.

## Task → command routing

Pick the verb, then read its command skill for the exact I/O contract. Each verb
is matched on `argv[1]` and dispatched in this order (first match wins):

| Want to… | Invoke | Routes to |
|---|---|---|
| Create / inspect / sign `.cja` archives | `cajeta archive <cmd>` | `cajeta::dispatchArchive` (`docs/ArchiveManagement.md`) |
| Show project / manifest info | `cajeta info [--manifest=<p>] [--properties]` | build-tool `infoCommand` |
| List / show build tasks | `cajeta tasks` / `cajeta task <name>` | build-tool |
| Scaffold a project | `cajeta init` | build-tool |
| Manage deps | `cajeta add` / `cajeta remove` / `cajeta upgrade` | build-tool |
| Build / test / run a declared task | `cajeta <task-name> [args]` | build-tool `runTaskCommand` (only if it `looksLikeTaskInvocation`) |
| Coverage / publish / trust / workspace | `cajeta coverage|publish|trust|workspace` | build-tool |
| Reproducibility / sandbox / toolchain | `cajeta verify-reproducible|sandbox-info|install|toolchain` | build-tool |
| Find / fetch skills | `cajeta search-skill <name>` / `list-skills` / `get-skills <uri>` | build-tool (`docs/SkillDiscovery.md`) |
| JIT-compile + run an entry point | `cajeta jit-run <source-root> <package.Class.method>` | `cajeta::jit::dispatchJitRun` |
| Provision a native dependency | `cajeta fetch` / `cajeta vendor` | `cajeta::dispatchNative` |
| Run the debug-adapter server (IDE drives it) | `cajeta dap` | `cajeta::dap::DapServer` over stdio |
| Generate docs | `cajeta doc <source-root> [...]` | `cajeta::doc::dispatchDoc` (same engine as the `cajetadoc` binary) |
| Manage the bundled IntelliJ plugin | `cajeta ide <install|uninstall|list>` | `cajeta::dispatchIde` |
| Print version + provenance | `cajeta --version [--verbose]` (or `-V`) | inline in `main` |
| **Compile sources** | `cajeta [flags] <entry-method> <source-root> <archive-root>` | `Compiler::compile` (the fall-through) |

Full dispatch order in `main`: `archive` → **build-tool subcommands** (incl. a
trailing `looksLikeTaskInvocation` task-name match) → `jit-run` → `fetch`/`vendor`
→ `dap` → `doc` → `ide` → `--version`/`-V` (scanned anywhere in argv) → raw
compile.

## Does NOT do

- **No single "build" verb in the bare compiler.** `cajeta build` is a *task*
  resolved by the build tool; the raw compile path takes three positionals, not a
  project. If you have a `cajeta.json` project, drive it through build-tool task
  verbs, not by hand-rolling compiler positionals.
- **The compiler's `--profile=<name>` is NOT a build mode** — it sets the active
  `@Profile` for component gating. Build *flavors* live in `--mode`/`--<feature>`.
- Unknown `--flags` are rejected (`unknown option`); unknown *verbs* are **not**
  errors — they fall through to compile and usually fail on the positional count.

## Compiler config precedence (the fall-through path)

Applied left-to-right, later wins (`src/main.cpp`, `docs/CompilerModes.md` §2):

1. **Flavor/mode** — `--mode=debug|debug-release|release|fast|minimal`, or alias
   flags `--debug`/`--debug-release`/`--release`/`--fast`/`--minimal`. Expands to a
   `CompilerFlags` via `CompilerFlags::defaultsForMode`. **Default `debug`** when
   none given (so a newcomer never ships a stripped binary by accident).
2. **Per-feature overrides** — `--bounds=on|off|trap`, `--null-checks`,
   `--overflow-checks`, `--source-tags`, `--opt`, `--live-set`, etc. Each mutates
   one field *after* the flavor expansion.
3. **Emit-mode-driven defaults applied last**: `--emit=exe` flips `--link-mode` to
   `lean` and `--tree-shake` to `on` unless you pinned them; amdgpu/vulkan
   `--xpu-backend` set a default `--xpu-arch` unless `--xpu-arch` is explicit.

The **build tool** owns `cajeta.json` profiles/flavor selection; it resolves them
and passes the chosen `--mode`/`--<feature>` flags *down* to this compiler path.

### Relevant `CAJETA_*` env vars (read via `getenv`)

- `CAJETA_CLASSPATH` — extra `.cja` classpath entries (merged with `--classpath`).
- `CAJETA_NATIVE_PATH` — native-library search path for `--emit=exe` linking.
- `CAJETA_REQUIRE_SIGNATURE=strict` — enforce archive signatures on install/dispatch.
- `CAJETA_NO_DISPATCH=1` — skip the toolchain re-exec in the launcher.
- `CAJETA_WILDCARDS`, `CAJETA_DUMP_IR`, `CAJETA_REUSE_FORCE_EMIT`, `CAJETA_JIT_GDB`
  — diagnostic/codegen toggles. (Most other `CAJETA_*` symbols are error IDs, not env.)

## Exit codes

Each dispatch function returns its own `int` exit code (forwarded as the process
status). The raw compile path returns `0` on success; `1` on usage error
(`< 3` positionals → prints usage), an unknown option, or a caught
`cajeta::Exception`/`std::exception` (message printed to stderr). `--help`/`-h`
and `--version` return `0`.

## Example

```sh
# Raw compile: release flavor, opt one debug feature back on, emit a native exe.
cajeta --release --source-tags=on --emit=exe \
       demo.App.run  src/main/cajeta  build/stdlib  -o build/app

# Build-tool surface (project-driven):
cajeta info --properties          # inspect the resolved manifest
cajeta search-skill cajeta/io/file   # find skills in resolved deps

# Run an entry point through the JIT, no archive produced:
cajeta jit-run src/main/cajeta demo.App.main
```

## Downward pointers

Read the per-command skills for exact I/O: the build-tool command group
(`cajeta::buildtool::dispatchBuildTool`, `src/cajeta/buildtool/BuildToolCommands.cpp`),
`cajeta archive` (`docs/ArchiveManagement.md`), `cajeta doc`
(`docs/Documentation.md`), `cajeta dap` (`docs/Debugging.md`), skills
(`docs/SkillDiscovery.md`), and modes/flags (`docs/CompilerModes.md`,
`docs/BuildTool.md`).
