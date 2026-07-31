---
id: cajeta-driver-compile
applies-to: [cajeta/toolchain/compile, cajeta-driver/compile]
title: cajeta AOT compile invocation
description: Direct cajeta compiler invocation — three positionals, --emit/--mode/safety/-o flags, exit codes.
---

# `cajeta` — direct AOT compile

The bare `cajeta` binary (no subcommand) IS the AOT compiler. It is the back end the
build tool wraps; you can drive it directly. Source: `src/main.cpp` (arg parsing) →
`Compiler::compile(entryMethod, sourceRoot, archiveRoot)` (`src/cajeta/compile/`).

## Invocation (exact)

```
cajeta [options] <entry-method> <source-root-path> <archive-root-path>
```

All **three positionals are required** — fewer than three prints usage and exits `1`.
They are positional-by-order, not flagged:

| Positional | Meaning |
|---|---|
| `<entry-method>` | Canonical entry point, dotted: `package.Class.method` (e.g. `demo.App.run`). |
| `<source-root-path>` | Root dir the compiler walks for `.cajeta` sources. |
| `<archive-root-path>` | Output root where emitted artifacts (and the resolved stdlib for linking) live. |

Anything matching `cajeta <subcommand> …` (`archive`, `ide`, `jit-run`, `dap`, `doc`,
`fetch`, `vendor`, plus build-tool verbs like `build`/`test`) is claimed before this
compile path — those are NOT this skill.

## Worked example

```sh
# Default: debug flavor, --emit=ir → exploded .ll per module under build/out
cajeta demo.App.run src build/out

# Release executable, link against a classpath, custom output path
cajeta --release --emit=exe \
  --classpath=lib/json.cja,lib/net.cja \
  -o build/app \
  demo.App.run src build/stdlib
```

## Output: `--emit` (default `ir`)

| `--emit` | Produces |
|---|---|
| `ir` (default) | Exploded text LLVM IR (`.ll`), one per module. |
| `obj` | Exploded native object files (`.o`), one per module. |
| `cja` | One `.cja` archive — project IR only (no stdlib, no deps). |
| `uber` | One `.cja` — project + stdlib + transitively-referenced deps (prune via `--prune-uber=off`). |
| `exe` | Linked executable (in-process lld). Implies defaults: `--link-mode=lean` and `--tree-shake=on` unless pinned. |

`-o <path>` sets the final artifact path (`-o` with no following arg → error, exit `1`).
`--target=<triple>` / `--cpu=<name>` / `--features=<list>` retarget (default: host).

## Mode / flavor + per-feature safety

Two layers, left-to-right (later wins). **Default flavor when none given is `debug`.**

1. Flavor: `--mode=debug|debug-release|release|fast|minimal`, or aliases `--debug`
   `--debug-release` `--release` `--fast` `--minimal`. Expands to a `CompilerFlags`
   profile (`CompilerFlags::defaultsForMode`, `src/cajeta/compile/CompilerMode.h`).
2. Per-feature overrides applied after the flavor:
   - `--bounds=on|off|trap`, `--null-checks=on|off|trap`,
     `--overflow-checks=on|off|wrapping`, `--live-set=strict|bounded|off`
   - `--source-tags`, `--poison-free`, `--drop-chain-validate`, `--ub-traps`,
     `--use-after-move-rt`, `--stack-trace-capture`, `--diag-hints`,
     `--profile-counters`, `--lazy-scope` — each `=on|off`
   - `--diag-verbosity=terse|normal|verbose`
   - `--opt=O0|O1|O2|O3` (IR opt for obj/exe; default O0, release/debug-release→O2, fast→O3)

Modes are a diagnostic/runtime-check envelope, **not a correctness gate** — debug and
release must behave identically; only checks the compiler can't prove safe, and
diagnostic metadata, differ. See `docs/CompilerModes.md` for the full profile table.

## Other notable flags

- `--classpath=a.cja,b.cja` — dependency `.cja` archives to ingest. Repeatable;
  comma-separates within each occurrence; empty entries skipped.
- `--skill-root=<dir>` — package root holding `skills/` to embed in the `.cja`.
  Defaults to the source root; the build tool passes the **project** root here (since the
  positional source root is the deeper `src/main/cajeta`).
- `--profile=<name>` — active `@Profile` for component gating (NOT a build mode).
- `--help`/`-h` → prints usage, exit `0`. `--version`/`-V` (pair with `--verbose`) → exit `0`.

## Exit codes & errors

| Condition | Code |
|---|---|
| Success | `0` |
| `--help`/`-h`, `--version`/`-V` | `0` |
| Usage error: `<3` positionals, unknown `--option`, bad flag value, `-o` with no arg | `1` |
| `cajeta::Exception` during compile | `1` — `cajeta: <errorId>: <message>` on stderr |
| Other `std::exception` | `1` — `cajeta: <what>` on stderr |

## Does NOT do

- No project discovery, dependency resolution, manifest reading, or task running — that
  is the build tool (`cajeta build`, `docs/BuildTool.md`). This path needs explicit
  positionals and `--classpath`.
- Does not auto-detect the entry point or the stdlib — you supply the entry method and
  the archive root holding the stdlib.
- Default emit is `ir`, NOT an executable; pass `--emit=exe` for a binary.
- `--profile` is not a build mode; use `--mode`/flavor for safety profiles.
