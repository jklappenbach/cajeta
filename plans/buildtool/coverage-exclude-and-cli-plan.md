# Plan — typed coverage exclusions + `cajeta coverage` CLI

> **COMPLETE (shippable scope).** C1 (typed plugin exclusions) + C2 (the `cajeta coverage
> ignore/list/remove` CLI + `ManifestEditor` mutators) are implemented, committed
> (`5bab003`, `704ee85`) and tested. C3 is a runtime-gated verification checkpoint (no
> separate work); C4 is a separate repo, out of scope. Deviation: the CLI landed inline in
> `BuildToolCommands.cpp` and tests in `ManifestEditorTests.cpp`, not the new files the
> touch-points table named.

Companion to [`build-tool-plan.md`](./build-tool-plan.md).
Replaces the deleted Phase 7e (`@nocoverage` source annotation).

## Context

Phase 7e originally proposed `@nocoverage("reason")` as a cajeta
source annotation that the compiler honored when emitting probes
under `--instrument=coverage`. That coupled a plugin-specific concept
to the language grammar — wrong layering once we accept that
`cajeta.coverage` is one of several possible coverage providers
(`acme.coverage`, etc.). Each provider defines its own opt-out shape;
the language stays neutral.

The opt-out moves into the plugin's `config` block in `cajeta.json`.
The HTML report's interactive "right-click → ignore" feature (the
follow-on slice from the user's mockup) shells out to a new
`cajeta coverage ignore` subcommand which uses the existing
`ManifestEditor` (JSONC-preserving) to append entries. The IDE
plugin and CI bots use the same surface.

## Decision: typed `exclude` entries

The plugin's existing `exclude` array — today a list of file globs —
becomes a list of typed objects:

```jsonc
"plugins": {
    "cajeta.coverage": {
        "version": "1.0.*",
        "config": {
            "grain": "line",
            "min":   80,
            "exclude": [
                { "kind": "file",
                  "pattern": "**/*_generated.cajeta",
                  "reason": "machine-generated; tested via integration" },
                { "kind": "package",
                  "pattern": "com.example.mock.*",
                  "reason": "test scaffolding" },
                { "kind": "symbol",
                  "pattern": "com.example.Foo.getCount",
                  "reason": "trivial accessor; tested implicitly via every use" }
            ]
        }
    }
}
```

Fields:

- `kind` — required; one of `file`, `package`, `symbol`.
- `pattern` — required; semantics per kind.
- `reason` — required; rejected when generic (`wip`, `todo`, `skip`,
  `fixme`, `tbd`, case-insensitive). Validation lives in the plugin's
  config parser, not in the compiler.

Kind semantics:

- `file` — glob over source paths, same as the v1 `exclude`'s entries.
  Drops the file from both numerator and denominator.
- `package` — glob over package names (`com.example.mock.*`). Drops
  every type whose package matches.
- `symbol` — exact-or-glob over fully qualified declaration names
  (`com.example.Foo.getCount`, `com.example.Bar.*`). Drops the named
  declaration's probes.

Backward compatibility: the existing form (string-only entries) is
read as `{ "kind": "file", "pattern": "<string>", "reason": "" }`.
The plugin warns about empty-reason entries on every run; the
warning's wording invites migration to typed form without forcing it.

## Decision: `cajeta coverage` CLI

A new subcommand group sits next to `cajeta upgrade` / `cajeta info`:

```
cajeta coverage ignore  --kind <file|package|symbol>  --pattern <p>  --reason <r>  [--yes]
cajeta coverage list    [--kind <k>]
cajeta coverage remove  --pattern <p>                                              [--yes]
```

Behaviors:

- `ignore` — append one typed `exclude` entry to
  `plugins.cajeta.coverage.config.exclude`. Refuses duplicates
  (same kind+pattern). Refuses generic reasons. Refuses when no
  `cajeta.coverage` plugin is declared. `--yes` skips the interactive
  confirmation when stdin is a TTY.
- `list` — prints the current exclude table, one row per entry, with
  the source line in the manifest cited so the user can find it.
- `remove` — deletes entries matching `pattern`. Errors when nothing
  matches. Always prompts unless `--yes`.

Same JSONC-preserving guarantee as `cajeta upgrade --melt`:
re-running an `ignore` that's a no-op writes nothing; running one
that adds an entry preserves every comment and the manifest's existing
indentation style.

## Touch points

| File                                                                          | Why                                                                |
|-------------------------------------------------------------------------------|--------------------------------------------------------------------|
| `build-tools/plugins/code-coverage/src/main/cajeta/cajeta/coverage/Exclude.cajeta` | New typed `Exclude` + `ExcludeKind` enum                       |
| `build-tools/plugins/code-coverage/src/main/cajeta/cajeta/coverage/ExcludeParser.cajeta` | Read plugin config block + reject generic reasons        |
| `build-tools/plugins/code-coverage/src/main/cajeta/cajeta/coverage/ExcludeMatcher.cajeta` | Per-kind match helpers; used by Report's filter pass    |
| `build-tools/plugins/code-coverage/src/main/cajeta/cajeta/coverage/Report.cajeta` | Plumb the resolved exclude list through the file/finding loops  |
| `build-tools/plugins/code-coverage/cajeta.json`                               | Update config block to show the typed form                         |
| `build-tools/plugins/code-coverage/README.md`                                 | Update the action contracts to mention typed `exclude`             |
| `src/cajeta/buildtool/ManifestEditor.{h,cpp}`                                 | New `appendCoverageExclude()` + `removeCoverageExclude()` mutators |
| `src/cajeta/buildtool/BuildToolCommands.{h,cpp}`                              | New `coverageCommand` dispatch + the three subcommands             |
| `src/cajeta/buildtool/cli/CoverageCli.{h,cpp}` (new)                          | Argv parsing for ignore/list/remove + TTY confirmation prompt      |
| `test/buildtool/CoverageCliTests.cpp` (new)                                   | Round-trip mutate, idempotence, generic-reason rejection           |
| `test/buildtool/ManifestEditorTests.cpp`                                      | Coverage-specific mutator coverage                                 |

The plugin-side files (`Exclude.cajeta` etc.) are written as cajeta
source today; they wait on the runtime to actually execute. The CLI
side (`ManifestEditor` + `CoverageCli`) runs immediately — no runtime
dependency — and gives the IDE a target to call into.

## Milestones

C1 and C2 can ship independently. C3 needs the plugin source to be
runnable (the runtime slice, deferred from #128).

### C1 — plugin: typed exclude shape (cajeta source)

- **C1.1** — `Exclude.cajeta`: typed entry record (`kind`, `pattern`,
  `reason`). `ExcludeKind` enum with `FILE`, `PACKAGE`, `SYMBOL`.
- **C1.2** — `ExcludeParser.cajeta`: parse the plugin config's
  `exclude` array. Accept string-form entries for back-compat with a
  warning. Reject generic reasons.
- **C1.3** — `ExcludeMatcher.cajeta`: per-kind match predicates. File
  glob uses the existing glob helper; package matcher splits on `.`
  and applies suffix-glob semantics; symbol matcher reuses package
  logic plus the trailing identifier segment.
- **C1.4** — `Report.cajeta`: wire `ExcludeMatcher` into the per-file
  + per-finding loops. Excluded probes get subtracted from both
  numerator and denominator; excluded findings get dropped from the
  emitted list.
- **C1.5** — `README.md` + sample `cajeta.json` updates demonstrating
  the new shape.

### C2 — CLI: `cajeta coverage` subcommand group

- **C2.1** — `ManifestEditor::appendCoverageExclude(manifestPath,
  kind, pattern, reason)`: locate (or create) the path
  `plugins.cajeta.coverage.config.exclude`, append a typed entry,
  preserve comments/whitespace. Refuse duplicates (same kind+pattern).
- **C2.2** — `ManifestEditor::removeCoverageExclude(manifestPath,
  pattern)`: find every matching entry, delete the JSONC object
  literal cleanly (including the comma).
- **C2.3** — `CoverageCli.cpp`: argv parser for `ignore`/`list`/
  `remove`. Generic-reason rejection on the CLI side too (defense in
  depth — the plugin parser is the spec, but the CLI catches mistakes
  before they hit disk).
- **C2.4** — `BuildToolCommands.cpp`: wire the new subcommand group
  into the existing `cajeta` dispatcher.
- **C2.5** — Tests: round-trip add/list/remove, idempotence (re-ignore
  is a no-op), generic-reason rejection, refusal when plugin isn't
  declared, TTY-prompt skip with `--yes`, behavior on a missing
  `exclude` array (create vs. append).

### C3 — wire-up: runtime calls plugin's `ExcludeMatcher`

Pending the runtime slice (#128). Once plugins can actually run, the
Report action picks up the typed exclude list at runtime and applies
it. C3 lands when #128 lands; no separate work for this milestone
beyond verifying.

### C4 — IDE plugin (separate repo)

Lives in `cajeta/ide-plugins/idea/`. Out of scope for this plan; the
plan it informs is "the IDE plugin shells out to `cajeta coverage
ignore`." Surface contract: the CLI is the API.

## Risks / open questions

1. **Glob dialect divergence between kinds.** Files use forward-slash
   path globs; packages use dotted-name globs. Two different matchers,
   easy to mix up. Mitigation: kind is required, the matcher routes
   on it, no auto-detection. Tests cover the wrong-kind case explicitly.

2. **`remove --pattern` is ambiguous when kinds differ.** A pattern
   like `com.example.*` could exist as both a `package` and a
   `symbol` entry. v1 deletes both and emits one message per
   deletion; the user sees what was removed. `remove --kind <k>
   --pattern <p>` form is the unambiguous alternative — added
   if/when the dual-kind case shows up in practice.

3. **Migration path for existing manifests.** Today's `exclude` is a
   `string[]`. Existing entries silently parse as `{kind:"file",
   reason:""}`. The plugin emits one warning per empty-reason entry
   on every run. We don't auto-migrate — `cajeta coverage migrate`
   could be added later if the warning noise becomes a problem.

4. **Generic-reason list.** `wip`, `todo`, `skip`, `fixme`, `tbd`.
   Defensible defaults; the list lives in `ExcludeParser`. The plugin
   may expose a config knob to extend it (e.g. add `"see PR-1234"` to
   the bad-reason list for a project that wants to enforce dated
   issue links). v1 ships the static list and revisits if asked.

5. **Refusal when plugin isn't declared.** `cajeta coverage ignore`
   needs `plugins.cajeta.coverage` to exist; otherwise we'd be
   silently creating a coverage config the user never opted into.
   The CLI errors with "no cajeta.coverage plugin declared in
   plugins; add one or run `cajeta plugin add cajeta.coverage`."

## Verification

Build: `cd build && cmake -G Ninja -DLLVM_DIR=/usr/lib/llvm-22/lib/cmake/llvm .. && ninja`.

Per-suite:
- `CoverageCliTests.*` — new CLI command coverage.
- `ManifestEditorTests.*` — typed-exclude mutator coverage.
- No full battery without explicit ask.

## Ordering / shippability

1. **C1** — plugin source for typed exclude. No runtime dependency;
   commits standalone.
2. **C2** — `cajeta coverage` CLI + ManifestEditor mutators. Also
   standalone; testable in isolation.
3. **C3** — verified when the runtime slice (#128) lands; no
   dedicated work.
4. **C4** — IDE plugin, separate repo, separate plan.

C1 and C2 commit in either order. Both before C3. C4 follows when
the team picks it up.
