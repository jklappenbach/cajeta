# Build-tool samples

Worked examples of cajeta build-tool projects. Each sample
demonstrates a different project shape from
[`cajeta-docs/BuildTool.md`](../../cajeta-docs/BuildTool.md):

| Sample                  | Shape                                                |
|-------------------------|------------------------------------------------------|
| [`basic/`](basic/)      | Single-package project — the simplest possible shape |
| [`workspace/`](workspace/) | Workspace with shared libraries + apps as members    |
| [`multi-binary/`](multi-binary/) | Single package producing several binaries from one source tree |
| [`melt/`](melt/)        | Melt-only package — curated version set, no source   |

## Caveat — compile pipeline not yet operational

These samples ship as **manifest + structure demonstrations**. The
build-tool's `build` action (Phase 5a, see
[`plan/build-tool-plan.md`](../../plan/build-tool-plan.md)) wraps
the cajeta compiler binary, but full source compilation against
resolved dependencies + IR caching lands in Phases 5b / 6b. So:

**Works today** (each sample's `run.sh` exercises these):

- `cajeta tasks` — list a project's tasks with descriptions.
- `cajeta task <name> --show` — render a task's resolved action
  sequence (with substitutions applied) without running it.
- `cajeta info --properties` — print the resolved property set.
- `cajeta info --write-lockfile` — persist the resolved state
  into `cajeta.lock`.
- Manifest validation: malformed manifests error with citations.

**Doesn't work end-to-end yet:**

- Actually running `cajeta build` end-to-end against the
  samples' source — the compiler integration's resolved-dep
  classpath plumbing lands in Phase 6b.
- Workspace member resolution by path (Phase 6c).
- Melts being imported as `settings.melts` (Phase 6c).

Each sample's `README.md` calls out specifically what its
`run.sh` exercises.

## Running

From any sample directory:

```
./run.sh
```

The script invokes the cajeta binary from `../../../build/src/cajeta`
(assuming a CMake build in the repo root's `build/`). Override
with the `CAJETA` env var:

```
CAJETA=/path/to/cajeta ./run.sh
```
