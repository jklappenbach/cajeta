# Cajeta Build-Tool Tool Window & JSONL View — Spec

> Status: **DRAFT for review** (design skill). Scopes two related IDE-plugin
> capabilities: a dockable **build-tool tool window** that mirrors IntelliJ's
> Gradle tool window against the `cajeta` build tool, and a **JSONL view** for
> the structured logs that build emits (and for `.jsonl` data generally).
> Companion of `ide-plugins/idea/Plan.md` (the plugin) and
> [`docs/BuildTool.md`](../BuildTool.md) (the build tool this surfaces).
> The plan lives at `agents/buildtool-widget-plan.md` once this is approved.

---

## 1. Definition

### 1.1 Purpose
Give Cajeta developers, inside IntelliJ IDEA, the same fluent build-orchestration
experience IntelliJ ships for Gradle: a dockable tool window listing the
project's tasks, run/debug on double-click or from a toolbar, a live execution
console, and the supporting affordances (refresh/sync, stop, linked projects,
run configurations, profile/flavor selection). The backend is the `cajeta`
build tool, whose model is **task-based** (`docs/BuildTool.md` §CLI surface):
tasks are declared in `cajeta.json` and run via bare `cajeta <task>`.

Because the build tool writes **per-action logs as JSONL** (`.cajeta/logs/`,
`docs/BuildTool.md` §project layout), this spec also defines a **JSONL view**:
structured rendering of newline-delimited JSON, surfaced both inside the tool
window's log/console pane and as a standalone `.jsonl` document viewer.

### 1.2 Scope (v1 — full Gradle-tool-window parity)
In scope for v1:
- A dockable, restorable tool window (the "Cajeta" widget).
- Task discovery via a new machine-readable `cajeta tasks --json` contract.
- A task tree grouped by source (manifest tasks vs built-in subcommands), with
  descriptions, parameters, and `depends-on` relationships.
- Task execution under **both** the Run and **Debug** executors (Debug reuses
  the Phase 2 `cajeta dap` / `XDebugProcess` plumbing).
- Run/stop lifecycle, re-run, and a live execution console.
- Structured **JSONL** rendering of build logs in the console, plus a
  standalone `.jsonl` document viewer — both with a raw-text toggle and
  field/level filtering.
- Toolbar parity: refresh/sync, run-arbitrary-task (typed-param dialog), stop,
  expand/collapse, grouping toggle, settings/link-project.
- **Linked projects / multi-root**: multiple `cajeta.json` roots in one IDE
  project, each its own tree node.
- **Run configurations & favorites**: a task (with bound params/profile/flavor)
  saved as a `CajetaTaskRunConfiguration` and pinned.
- **Profiles, flavors, properties**: per-run selection of `--profile`, flavor
  (`--release/--debug/--fast`), and `-P key=value` overrides.
- **Background auto-sync**: re-discover tasks when a watched `cajeta.json`
  changes.
- A `buildToolPath` setting (distinct from the existing compiler
  `compilerPath`).

### 1.3 Problem
Today the plugin can lint and debug, but a developer must drop to a terminal to
build, test, or run any task, and build output is an undifferentiated text
stream even though the tool emits structured JSONL. There is no in-IDE view of
what tasks a project defines, no one-click run/debug, and no way to read or
filter the structured logs the build already produces.

### 1.4 Constraints & dependencies
- **Backend contract.** Requires a new `cajeta tasks --json` mode on the build
  tool (a build-tool-side prerequisite, analogous to the lint-JSON path Phase 1
  needed). Defined normatively in §3.
- **Debug reuse.** Run-under-debug reuses the existing
  `debugger/` package (`CajetaDapLauncher`, `CajetaDebugProcess`,
  `CajetaProgramRunner`) and the `cajeta dap` server; this spec adds the
  task→launch wiring, not a new debug backend.
- **Platform.** IntelliJ Platform 2024.2+ (matches the plugin's existing
  `sinceBuild`); tool window via `com.intellij.toolWindow`, execution via the
  Execution/RunConfiguration framework, JSONL editor via `FileType` +
  `FileEditorProvider`.
- **Behavioral cores stay plain JVM** (no `com.intellij.*`) so they unit-test
  without a platform fixture — consistent with the debugger checkpoints
  (FR-8.4 in the debug FR doc). Platform classes are thin delegates.
- **Binaries.** The `cajeta` build tool is distinct from the compiler at
  `build/src/cajeta`; the widget uses `buildToolPath`, defaulting to `cajeta`
  on `PATH`.

### 1.5 Non-goals (v1)
- Reimplementing the build graph / dependency resolution in Kotlin — the plugin
  always shells out to `cajeta`; it never resolves tasks itself.
- A general JSON (not JSONL) tree editor, schema validation, or JSONPath query
  UI — the JSONL view targets newline-delimited records, not arbitrary JSON.
- Editing `cajeta.json` through the tool window (add/remove task via UI) — the
  manifest is edited as text; the tree is read-only over it.
- Authoring build-tool plugins/actions from the IDE.
- Remote/CI execution — v1 runs the local `cajeta` binary only.

---

## 2. Tool window shell & docking

### 2.1 Requirements
A dockable tool window registered as "Cajeta" (build-tool icon), defaulting to
the left/bottom dock like Gradle's, restorable across sessions, available when
the open project contains at least one `cajeta.json`.

### 2.2 Use cases
- **2.2.1** As a developer, when I open a project containing a `cajeta.json`,
  then a "Cajeta" tool-window stripe button appears and clicking it opens the
  widget docked.
- **2.2.2** As a developer, when I drag/redock or hide the tool window, then its
  placement and visibility persist across IDE restarts (standard tool-window
  state).
- **2.2.3** As a developer, when the project has **no** `cajeta.json`, then the
  tool window is absent (not an empty shell), and appears automatically once a
  `cajeta.json` is created/detected.

---

## 3. Task discovery & the `cajeta tasks --json` contract

### 3.1 Requirements
The plugin discovers tasks by invoking `cajeta tasks --json --manifest=<path>`
and parsing a stable JSON document. The contract (build-tool-side prerequisite):

```jsonc
{
  "manifest": "/abs/path/cajeta.json",
  "tasks": [
    { "name": "build", "description": "Compile + link the project",
      "dependsOn": ["check"],
      "params": [ { "name": "flavor", "type": "string", "default": "debug",
                    "required": false, "doc": "Build flavor" } ] }
  ],
  "builtins": [
    { "name": "tasks", "description": "List task names" },
    { "name": "info",  "description": "Print dependency tree / capabilities" }
  ]
}
```

- **3.1.1** Each manifest task carries `name`, optional `description`,
  `dependsOn` (from the manifest DAG), and typed `params` (name/type/default/
  required/doc) per the `docs/BuildTool.md` task-field reference.
- **3.1.2** `builtins` enumerates the tool's built-in subcommands (init, add,
  info, …) the IDE may surface as runnable, separately from manifest tasks.
- **3.1.3** Parsing is tolerant: unknown fields are ignored (forward-compat),
  and a non-zero exit or malformed output degrades gracefully (§15.3) without
  throwing into the EDT.
- **3.1.4** The discovery core (JSON → task model) is plain JVM and
  unit-tested against fixtures, independent of any process spawn.

### 3.2 Use cases
- **3.2.1** As a developer, when the tool window first opens, then the plugin
  runs `cajeta tasks --json` off the EDT and populates the tree from the parsed
  result within a couple seconds, showing a progress indicator meanwhile.
- **3.2.2** As a developer, when my `cajeta.json` defines a task with a
  `description`, then that text appears as the tree node's tooltip/secondary
  text.
- **3.2.3** As a build-tool maintainer, when I add a field to the JSON contract,
  then an older plugin still parses successfully (ignores the unknown field).

---

## 4. The task tree

### 4.1 Requirements
A tree presenting, per linked project root: a **Tasks** group (manifest tasks)
and a **Built-in** group (subcommands), each node showing name + description,
with `depends-on` viewable. Search/filter (speed-search) over names and
descriptions. Double-click or Enter runs the selected task. Context menu offers
Run, Debug, Run with arguments…, Save as Run Configuration, and "Open in
manifest" (navigate to the task's source range in `cajeta.json`).

### 4.2 Use cases
- **4.2.1** As a developer, when I expand a project node, then I see Tasks and
  Built-in groups with their entries sorted alphabetically.
- **4.2.2** As a developer, when I type into the tree's speed-search, then nodes
  filter live by name and description substring (typo-tolerant not required).
- **4.2.3** As a developer, when I double-click `build`, then it runs (§5) under
  the Run executor.
- **4.2.4** As a developer, when I right-click a task and choose "Open in
  manifest", then `cajeta.json` opens with the caret on that task's definition.
- **4.2.5** As a developer, when I select a task with `depends-on`, then the
  dependency tasks are discoverable (child nodes or tooltip), matching the
  manifest DAG.

---

## 5. Task execution — run & debug

### 5.1 Requirements
Running a task spawns `cajeta <task> [flags]` via a `CajetaTaskRunConfiguration`
integrated with the IDE Execution framework, so it appears in the Run/Debug
toolbar, has a tool-window console tab, and supports stop and re-run. Under the
**Debug** executor, the task is launched through the existing
`CajetaProgramRunner` → `cajeta dap` path so breakpoints/stepping work for the
task's runnable artifact (reusing Phase 2). Flags applied: selected
profile/flavor/properties (§12) and bound params (§11).

### 5.2 Use cases
- **5.2.1** As a developer, when I run a task, then a console tab opens under
  the Run tool window streaming the process output, with a red Stop affordance
  while it runs and a green Re-run when it finishes.
- **5.2.2** As a developer, when I choose **Debug** on a task that produces a
  runnable artifact, then the process launches under `cajeta dap`, my Cajeta
  breakpoints bind, and the debugger UI (stack/variables/fibers) drives it —
  identical to a Phase 2 debug session.
- **5.2.3** As a developer, when I press Stop, then the spawned `cajeta` process
  (and its children) terminate and the console shows the exit/cancel state.
- **5.2.4** As a developer, when a task exits non-zero, then the console marks
  failure and (where the tool emits structured error records) the JSONL view
  (§7) highlights the failing action.

---

## 6. Run/stop lifecycle & execution console

### 6.1 Requirements
Each execution owns a console with: the raw process stream, a Stop/Re-run
toolbar, and — when the run produces JSONL (stdout JSONL stream and/or
`.cajeta/logs/*.jsonl`) — a toggle to the structured JSONL view (§7). Multiple
concurrent runs each get their own tab. Output is incremental (streamed, not
buffered-to-end).

### 6.2 Use cases
- **6.2.1** As a developer, when a long build runs, then output appears
  incrementally as the process emits it (no wait-for-completion).
- **6.2.2** As a developer, when two tasks run at once, then each has its own
  console tab and Stop controls; stopping one does not affect the other.
- **6.2.3** As a developer, when a run finishes, then I can Re-run it from the
  console without re-selecting in the tree.

---

## 7. JSONL log rendering (console surface)

### 7.1 Requirements
When a run's output (or a chosen `.cajeta/logs/*.jsonl`) is JSONL, the console
offers a **structured** mode: one row per record, columns derived from common
keys (e.g. `ts`, `level`, `action`, `msg`), nested objects expandable, with a
**raw-text toggle** and filtering by level and by field substring. Rendering is
the *same engine* as the standalone viewer (§8) — single source of truth.

### 7.2 Use cases
- **7.2.1** As a developer, when a build emits JSONL log lines, then I can
  switch the console to structured mode and read one row per record with
  level/action/message columns.
- **7.2.2** As a developer, when I filter to `level >= warn`, then only matching
  records show; toggling back to raw shows the verbatim stream.
- **7.2.3** As a developer, when a record has a nested object/array, then I can
  expand it inline without leaving the console.
- **7.2.4** As a developer, when a line is **not** valid JSON (interleaved plain
  text), then it renders as a raw passthrough row, never dropped or erroring.

---

## 8. Standalone JSONL document viewer

### 8.1 Requirements
A `.jsonl` (and `.jsonlines`/`.ndjson`) file type with a `FileEditorProvider`
offering a structured view alongside the text editor (split or tab), reusing the
§7 rendering engine: per-record rows, expandable nesting, level/field filtering,
raw toggle. Read-oriented; large files stream/window rather than loading whole.

### 8.2 Use cases
- **8.2.1** As a developer, when I open a `.jsonl` file, then I get a structured
  view (and can switch to plain text) — the same rendering as the console.
- **8.2.2** As a developer, when I open a multi-hundred-MB `.jsonl`, then it
  opens responsively (windowed/streamed) without OOM.
- **8.2.3** As a developer, when I open a `.cajeta/logs/<action>.jsonl` from the
  project tree, then it renders structured, matching what the console showed for
  that action.

---

## 9. Toolbar & actions

### 9.1 Requirements
Tool-window toolbar mirroring Gradle's: **Refresh/Sync** (re-run discovery),
**Run task…** (pick/type a task → typed-param dialog), **Stop** (active runs),
**Expand all / Collapse all**, **Grouping toggle** (group by project /
flat), **Link project…** (§10), **Settings** (jump to the Cajeta settings page).

### 9.2 Use cases
- **9.2.1** As a developer, when I click Refresh, then task discovery re-runs and
  the tree updates (added/removed tasks reflected), with a spinner during.
- **9.2.2** As a developer, when I click "Run task…", then a searchable picker
  lets me choose a task and fill its typed params, then runs it.
- **9.2.3** As a developer, when runs are active and I click Stop, then I can
  cancel one or all active runs.
- **9.2.4** As a developer, when I toggle grouping, then the tree switches
  between grouped-by-project and flat presentations and remembers my choice.

---

## 10. Linked projects / multi-root

### 10.1 Requirements
Support multiple `cajeta.json` roots in one IDE project (auto-detected on import
+ manually linkable/unlinkable). Each root is a top-level tree node with its own
tasks, discovery, and run context. Workspaces (`docs/BuildTool.md` §workspaces)
appear as a root whose members are child roots.

### 10.2 Use cases
- **10.2.1** As a developer, when my IDE project contains two `cajeta.json`
  roots, then both appear as sibling top-level nodes, each with its own tasks.
- **10.2.2** As a developer, when I "Link project…" and pick a `cajeta.json`
  outside the auto-detected set, then it's added as a root and persists.
- **10.2.3** As a developer, when I unlink a root, then its node and watchers are
  removed and the choice persists.
- **10.2.4** As a developer, when I open a workspace manifest, then its member
  packages appear as child roots and each member's tasks are runnable.

---

## 11. Run configurations & favorites

### 11.1 Requirements
A `CajetaTaskRunConfiguration` (type + factory + editor) capturing task, root,
bound params, profile/flavor/properties. Creatable from a task ("Save as Run
Configuration"), editable in Run/Debug Configurations, and pin-able as a
favorite surfaced at the top of the tool window.

### 11.2 Use cases
- **11.2.1** As a developer, when I "Save as Run Configuration" on a task with
  params filled, then a reusable config appears in the Run/Debug dropdown with
  those bindings.
- **11.2.2** As a developer, when I edit that config's params/flavor in the
  Run/Debug Configurations dialog, then subsequent runs use the new values.
- **11.2.3** As a developer, when I mark a config as favorite, then it pins to a
  Favorites section at the top of the tool window for one-click run/debug.

---

## 12. Profiles, flavors & properties

### 12.1 Requirements
Per-run selection of `--profile=<name>`, flavor (`--release/--debug/--fast`),
and zero or more `-P key=value` overrides (`docs/BuildTool.md` §profiles,
§properties, §build flavors). A tool-window selector sets the active
profile/flavor used by double-click runs; the Run-with-args dialog and run
configs can override per-run.

### 12.2 Use cases
- **12.2.1** As a developer, when I set the active profile to `integration` in
  the tool window, then double-clicking `test` runs
  `cajeta test --profile=integration`.
- **12.2.2** As a developer, when I run with flavor `release`, then `--release`
  propagates as the `${flavor}` property to the task's build action.
- **12.2.3** As a developer, when I add `-P stack-version=1.5.0` in the run
  dialog, then it is passed through and visible in the launched command line.

---

## 13. Background auto-sync

### 13.1 Requirements
Watch each linked `cajeta.json` (and lockfile where relevant); on change, offer
or auto-run discovery to refresh the tree, debounced. A non-intrusive banner
("Cajeta projects need to be reloaded") with a Reload action, plus an
auto-reload setting (default: prompt, like Gradle).

### 13.2 Use cases
- **13.2.1** As a developer, when I edit `cajeta.json` to add a task, then a
  reload banner appears; clicking Reload re-discovers and the new task shows.
- **13.2.2** As a developer, when I enable auto-reload, then manifest edits
  refresh the tree automatically (debounced) without the banner.
- **13.2.3** As a developer, when discovery fails after an edit (bad manifest),
  then the error surfaces in the banner/log, and the previous tree stays usable.

---

## 14. Settings

### 14.1 Requirements
Extend the Cajeta settings page with: `buildToolPath` (default `cajeta` on
`PATH`, validated executable), auto-reload toggle, default profile/flavor,
JSONL-view defaults (structured vs raw, default level filter). All persisted via
the existing `CajetaSettings` `PersistentStateComponent`.

### 14.2 Use cases
- **14.2.1** As a developer, when I set `buildToolPath` to a non-default
  location, then discovery and runs use that binary; an invalid path is flagged
  in settings and surfaced (not a silent failure) on first use.
- **14.2.2** As a developer, when I change the default JSONL view to "structured",
  then newly opened `.jsonl` files and consoles default to structured mode.

---

## 15. Non-functional requirements

- **15.1 Performance.** Discovery and runs never block the EDT; the tree renders
  incrementally; the JSONL view windows large files (§8.2.2). Typing in tree
  speed-search stays responsive.
- **15.2 Cross-platform.** Linux/macOS/Windows, consistent with the existing
  debugger; process spawning reuses the `CajetacRunner`/`CajetaDapLauncher`
  patterns (PATH/DLL handling on Windows).
- **15.3 Graceful degradation.** Missing/invalid `buildToolPath`, non-zero
  discovery, malformed JSON, or a killed run never error-dialog or hang the IDE
  — they log, show an inline/banner message, and keep prior state usable
  (mirrors the lint/debug resilience already shipped).
- **15.4 Testability.** Discovery parsing, the task model, JSONL parsing/render
  model, and command-line construction are plain-JVM cores with direct unit
  tests; platform classes (tool window, console, editor provider, run config)
  are thin and integration-tested against the real `cajeta` binary where it
  crosses the process boundary, skipping cleanly when the binary is absent
  (the `assumeTrue` pattern the debugger integration tests use).
- **15.5 Single source of truth.** Console JSONL rendering and the standalone
  viewer share one engine (§7.1); the task model has one parser (§3.1.4).

---

## 16. Build-tool-side prerequisite (cross-repo)

`cajeta tasks --json` (§3) does not exist yet — today `cajeta tasks` prints a
human listing (`docs/BuildTool.md` §CLI surface). Adding the JSON mode is a
build-tool commit (in `src/cajeta/buildtool/`), analogous to the lint-JSON path
the linting tier needed. The plugin's discovery layer is written against the §3
contract and can ship behind it; until it lands, a degraded fallback MAY parse
`cajeta.json`'s `tasks` block directly (clearly marked temporary, mirroring the
degraded-lint precedent). This prerequisite is tracked as the first unit in the
plan.
