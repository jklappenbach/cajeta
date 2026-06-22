---
id: cajeta-driver-tasks
applies-to: [cajeta-driver/build, cajeta-driver/run, cajeta-driver/clean, cajeta-driver/test]
title: cajeta task invocation (build/run/clean/test and any manifest task)
description: How `cajeta <name>` resolves and runs a task defined in ./cajeta.json, with property/param/flavor/profile overrides and cross-member form.
---

# `cajeta <task>` — run a manifest-defined task

`build`, `run`, `clean`, `test` are **not built-in subcommands**. They are ordinary
task names that must exist in the `tasks` block of `./cajeta.json`. `cajeta build`
works only because the convention is to define a task literally named `build`; the
same machinery runs `cajeta deploy` or any other name. The driver looks up
`argv[1]` against the manifest's `tasks` map (`looksLikeTaskInvocation` in
`src/cajeta/buildtool/BuildToolCommands.cpp`) and, on a hit, runs it via
`runTaskCommand`.

| You want to… | Invoke | Requires in `./cajeta.json` |
|---|---|---|
| Build | `cajeta build` | `tasks.build` |
| Run the built artifact | `cajeta run` | `tasks.run` |
| Clean outputs | `cajeta clean` | `tasks.clean` |
| Test | `cajeta test` | `tasks.test` |
| Any custom task | `cajeta <name>` | `tasks.<name>` |
| A sibling member's task | `cajeta <member>:<name>` | a workspace ancestor + that member |
| List available tasks | `cajeta tasks` | — |
| Inspect a task without running | `cajeta task <name> --show` | `tasks.<name>` |

If `argv[1]` is not a reserved subcommand (`info tasks task init add remove upgrade
coverage publish trust workspace verify-reproducible sandbox-info install toolchain`
and the `*-skill` commands), does not start with `-`, and `./cajeta.json` exists with
a matching key under `tasks`, it is dispatched as a task. **Otherwise the argument
falls through to the compiler back-end** (the `cajeta` binary is the compiler today),
which treats it as an input file. So `cajeta build` with no manifest, or `cajeta
buld` (typo), is handed to the compiler — you get a compiler "error reading" message,
not "unknown task." There is no fuzzy task matching here.

## Flags (overrides)

Parsed by `runTaskCommand`. Two distinct override channels:

- **Properties** (manifest-wide `properties` block): `-P NAME=VALUE` (space-separated)
  or `--property=NAME=VALUE`.
- **Task params** (the task's own `params` spec, read as `${params.NAME}`):
  `-p NAME=VALUE` (space-separated) or `--param=NAME=VALUE`.
- `--flavor=NAME`, `--profile=NAME` — override active build flavor/profile.
- `--manifest=<path>` — use a manifest other than `./cajeta.json`.

Note the case split: capital `-P` = property, lowercase `-p` = param. Env-var
overrides are loaded first (`loadEnvOverrides`), then CLI overrides layer on top.

## Cross-member form `<member>:<task>`

`cajeta <member>:<task>` reroutes the lookup to a sibling member's manifest before
any resolution. It requires a `workspace` block on an ancestor directory of the cwd;
the member is matched by short name. Errors are structured: a bad form (`:x`, `x:`)
reports the expected shape; no workspace ancestor or an unknown member names what was
found (`no workspace member named 'X' (known: a, b)`).

## Example

```sh
# samples/tour defines tasks: build, run, clean
cd samples/tour
cajeta tasks                      # see what's defined
cajeta build                      # run the build task
cajeta run -p profile=dev         # task param override
cajeta build -P version=1.2.0 --flavor=release   # property + flavor override
cajeta task build --show          # print resolved action sequence, run nothing
```

A task that declares `outputs` (e.g. `{ "path": "${art.path}" }`) prints them after
a successful run:

```
Task 'build' outputs:
  path = build/app
  sha256 = 9f2c...
```

Tasks with no declared outputs print nothing extra. The action sequence itself
(`build`, `exec`, `clean`, `test`, plugin actions) produces its own stdout/stderr.

## Exit codes & errors

- `0` — task ran successfully.
- `1` — any failure: manifest missing/malformed, undefined property/param,
  task-graph cycle or undefined `depends-on` (validated up front by
  `validateTaskGraph` before anything runs), unknown flag, action failure, or a
  bad cross-member ref. The message is prefixed `cajeta <task>: ...` on stderr.
- A name with no matching task does **not** error here — it falls through to the
  compiler (see above).

## What this does NOT do

- No hardcoded build/run/clean/test logic — behavior is whatever the manifest's
  task defines. An empty `tasks` block means `cajeta build` reaches the compiler.
- Does not create or scaffold tasks — use `cajeta init` for a starter manifest.
- Does not edit dependencies — see `cajeta add`/`remove`/`upgrade`.
- `cajeta task <name>` has only `--show` today; it does not run the task (use bare
  `cajeta <name>` to run).
