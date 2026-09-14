# Spec — lint-own-archive-classpath

Status: draft (filed 2026-09-13)

## 1. Definition

### 1.1 Purpose
Make IDE lint resolve the **project's own types** in a project whose tests live in
a source root separate from its main sources. Today they resolve for the build and
not for lint, so the editor reports types that compile perfectly as unresolved.

### 1.2 The problem, as measured
`cajeta-http` has two source roots: `src/main/cajeta` (the library) and `test/src`
(its tests). Opening `test/src/dev/cajeta/http/test/ServerTests.cajeta` underlines
`Router`, `HttpResponse`, `HttpSerializer` and others in red. The suite builds and
**1434 tests pass** — the types are fine.

`CajetacRunner.sourceRootOf` (`CajetacRunner.kt:115`) derives the lint source root by
walking up from the file according to its package declaration. For that file and
`package dev.cajeta.http.test` it yields `test/src`, which does not contain the
library. Measured against `ServerTests.cajeta`:

| lint invocation | unresolved types |
|---|---|
| `--source-root test/src` (what the plugin passes today) | 13 |
| `--source-root <project root>` | 0 |
| `--source-root test/src --classpath=<own .cja>` | **0** |

### 1.3 The actual gap
This is **not** a multi-root problem. `run-tests.sh:176` compiles the suite as
`<entry> test/src build/test --classpath=$ART,$CODEC_CJA` — one source root plus the
project's **own built archive**. The build never needs a second source root, and
neither does lint.

The plugin already passes a classpath, but
`CajetaSourceMountGlue.dependencyArchives` (`:51`) scans only
`.cajeta/cache/artifacts` — the **dependency** cache. The project's own
`build/archive/*.cja` is never on it. That one missing entry is the whole defect.

### 1.4 Scope
The own archive is added to the classpath on every lint path: the per-edit
annotator (one-shot), the warm `--lint-server`, and the whole-root `--emit-xref`
export.

### 1.5 Constraints
- **1.5.1** No compiler change. The compiler already accepts `--source-root` plus
  `--classpath`; only what the plugin puts on the classpath changes.
- **1.5.2** No added tree scan. Widening the source root to the project root
  resolves too, but costs ~50% more lint wall time (10.5 s → 15.7 s measured cold
  under load) and pulls in `build/`, `tmp/` and `samples/`.
- **1.5.3** Resolution must not depend on a build the developer did not ask for.

### 1.6 Non-goals
- **1.6.1** Multi-root source **resolution**. `lint-source-root-spec` §1.4 defers
  it, and repeating `--source-root` does not union roots — the compiler takes the
  last one (measured: 3 errors, all `Check`). Nothing here changes that: one root
  plus a classpath resolves correctly, which is the whole point of §2.

  **Amended 2026-09-13.** This non-goal was read too broadly when the spec was
  written. Resolution and **coverage** are different questions, and only the first
  is out of scope. A file resolves against one root, but the xref export must
  still *visit* every root or the files in the unvisited ones are absent from the
  index entirely. Coverage is now §8, and it is in scope.
- **1.6.2** Which xref **relations** are emitted (call edges, field references).
  That is `xref-lint-emission-gap`. This spec governs whether the project's own
  types resolve **at all**; that plan governs what is emitted once they do. The two
  meet only at the whole-root export.
- **1.6.3** Building the project on the developer's behalf.

## 2. The resolution input

### 2.1 When a lint request is made for a file in a project that declares a build
artifact, that artifact is on the lint classpath.

### 2.2 When the own archive is on the classpath, references from a test-root file
to the project's own types resolve, and no `CAJETA_ERROR_UNRESOLVED_TYPE` is
reported for them.

### 2.3 When the own archive is added, the source root is unchanged — it stays the
package-derived root from `sourceRootOf`.

### 2.4 When the project declares no build artifact, the classpath is the
dependency set alone and lint behaves as it does today.

## 3. Archive discovery

### 3.1 When the archive path is needed, it is obtained from
`cajeta artifact-path`, which prints the declared absolute path without building
and exits non-zero when the manifest declares no artifact.

### 3.2 When a build directory holds several archives, the one named by
`artifact-path` is used and the others are ignored. Globbing is forbidden:
`cajeta-http/build/archive` holds `0.1.3`, `0.1.4` and `0.2.0`, and a glob picks a
stale one. (Measured 2026-09-13 — a `head -1` glob selected `0.1.3` against a
`0.2.0` manifest.)

### 3.3 When `artifact-path` names a file that does not exist on disk, that is the
absent case (§4), not an error.

### 3.4 When the archive is resolved, it is appended to the dependency archives
rather than replacing them, so dependency types keep resolving.

## 4. Absent archive — visible degradation

### 4.1 When the project has never been built, lint runs without the own-archive
entry rather than failing or building anything.

### 4.2 When lint runs without the own-archive entry, the IDE states the reason and
the remedy — that the project is not built, so its own types cannot resolve, and
that `cajeta build` fixes it.

### 4.3 When the reason is shown, it is attributed to project state and not to the
edited file, so it does not read as a defect in the source.

### 4.4 When the project is subsequently built, the next lint picks the archive up
with no IDE restart and no cache invalidation.

## 5. Staleness

### 5.1 When the archive is older than the newest main source, it is still used for
resolution.

### 5.2 When the archive is provably stale by §5.1, a weak warning says so and names
the remedy, because edits to main sources do not reach test-file lint until a
rebuild.

### 5.3 When the archive is current, no staleness warning is emitted.

### 5.4 When staleness is judged, it is judged by comparing the archive's mtime
against the newest main-source mtime — not by version string. A `0.1.3` archive
resolved a `0.2.0` tree correctly because the types had not moved, so a version
mismatch is not by itself a defect.

## 6. Coverage across the three lint paths

### 6.1 When the per-edit annotator lints a buffer, the own archive is on the
classpath.

### 6.2 When the warm `--lint-server` is enabled, the own archive is in the
server's start-time classpath. Source root and classpath are fixed at spawn and are
not per-request (`LintServerCore.kt:19`), so an annotator-only fix would not hold
whenever the daemon is on.

### 6.3 When the server's start-time context is stale — the archive appeared, or
changed, after spawn — the server is restarted so the new context takes effect.

### 6.4 When the whole-root `--emit-xref` export runs, the own archive is on its
classpath, so the shard it produces carries the project's own symbols.

This is necessary and **not sufficient** — it fixes what the export can resolve,
not which files it covers. See §8.

### 6.5 When a per-edit lint stream overwrites the whole-root shard, the project's
own Ctrl-click targets survive. Without the archive those references are dropped as
dangling, and the per-edit stream erases them from the shard — the same mechanism
already documented for dependency targets at `CajetaLintAnnotator.kt:20-24`.

## 7. Acceptance

### 7.1 When `ServerTests.cajeta` is opened in `cajeta-http` with the project built,
no type in it is underlined as unresolved.

### 7.2 When the same file is linted from the CLI as the plugin invokes it, the
unresolved-type count is 0.

### 7.3 When the project is unbuilt, the count is unchanged from today's behaviour
and §4.2's reason is visible.

### 7.4 When a single-root project is linted, behaviour is unchanged — no
regression for projects that never had this problem.

## 8. Export coverage — every source root

Added 2026-09-13, after the §2 fix shipped and Ctrl-click stayed dead.

`CajetaXrefRebuildAction` exports `CajetaRoots.conventionalSourceRoot(base)`,
which is `<base>/src/main/cajeta` when that exists. A project whose tests live
elsewhere therefore has **no index for its tests at all** — not a weaker index, an
absent one. Measured on `cajeta-http` with the §2 fix in place and the project
built: the exported shard names `HttpSerializer` 587 times and `ServerTests` **0**
times. Every import in that file is dead because the file it is clicking *from* was
never visited.

There is no convention to lean on. Surveyed 2026-09-13: `cajeta-http` uses
`test/src`; `cajeta-codec` and `cajeta-logging` use `src/test/cajeta`. No manifest
declares a test root.

### 8.1 When the index is rebuilt, every source root the project has is exported,
not only the conventional main root.

### 8.2 When source roots are discovered, they are derived the way lint derives
one: a `.cajeta` file's declared `package a.b.c` is stripped from the tail of its
path, and what remains is a root. The distinct roots so found are the set to
export. This is the same rule as `CajetacRunner.sourceRootOf`, so what the export
covers and what lint resolves against agree by construction rather than by two
conventions kept in step by hand.

### 8.3 When roots are discovered, `build/`, `tmp/`, `.cajeta/` and any other
generated or vendored tree are excluded, so a stale copy of the sources under
`build/` is never mistaken for a root.

### 8.4 When a file declares no package, or its on-disk layout does not match its
package, its own directory is the root — the same fallback `sourceRootOf` takes.

### 8.5 When several roots are exported, their records accumulate rather than
replace one another. Shards are written per source file
(`CajetaXrefShards.shardName(sourceRelPath)`), so two export passes over different
roots write disjoint shards and no merge step is needed.

### 8.6 When a project has exactly one source root, exactly one export runs and the
behaviour is unchanged from today.

### 8.7 When a root is exported, the own archive is on its classpath (§6.4), so a
test-root file resolves the project's own types as Ctrl-click targets.

### 8.8 When the rebuild reports progress, it names the root being exported, since
a multi-root rebuild is longer than a single-root one and a silent pause reads as
a hang.

### 8.9 Acceptance — Ctrl-click from an import in
`cajeta-http/test/src/.../ServerTests.cajeta` lands in the importing type's
declaration, and the exported shard names `ServerTests`.
