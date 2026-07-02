# Cajeta IDEA/CLion Plugin — Build Tool Window Routing (spec)

## 1. Definition

### 1.1 Purpose
Route **build-family** Cajeta task execution into the IDE's **native Build
tool window** (`com.intellij.build` — the same surface CMake/Gradle/Maven use),
instead of the Run tool window it uses today, and surface compiler diagnostics
as clickable problems in that window and the Problems view.

### 1.2 Problem
Every task launch — gutter, the "Cajeta Build" tool window (double-click /
Run Task), saved configs — funnels through
`CajetaTaskLauncher` → `ProgramRunnerUtil.executeConfiguration(settings,
DefaultRunExecutor)`. The Run executor always docks its console in the **Run**
tool window. So a `compile`/`package` "build" lands in *Run*, which mismatches a
CLion user's mental model ("Build" belongs in the Build window) and forgoes the
Build window's event tree, progress, and Problems integration.

### 1.3 Scope
- Classify each launch as **build-routed** or **run-routed**.
- For build-routed launches: drive the native Build tool window (start / output
  / finish events, progress, a stoppable process).
- Parse compiler stderr/stdout into structured **problem** events linked to
  source, mirrored into the Problems view.
- Leave run-routed and **all debug** launches exactly as they are.

### 1.4 Constraints
- **1.4.1** No change to run/exec or debug launch paths — `run` and any
  dap-debuggable task keep using the Run/Debug windows (they execute a program;
  their stdout is program output, not build output).
- **1.4.2** Reuse the existing degraded-mode diagnostic vocabulary
  (`lint/Diagnostic`, `CajetacRunner.WARNING_RE`); do not invent a second
  parser. Precise ranges/positions upgrade for free when
  `cajeta --diag-format=json` lands (tracked with the existing degraded-lint
  follow-up).
- **1.4.3** Process spawning stays off the EDT (as `CajetaBuildRunner` already
  requires); build events are posted on the correct threads for the Build API.
- **1.4.4** Cancellation must actually kill the child process (`destroyForcibly`,
  as `CajetaBuildRunner.spawn` does today) and report the build as cancelled.

### 1.5 Non-goals
- **1.5.1** No structured/JSON diagnostic protocol work — this consumes whatever
  the compiler prints today (regex), same posture as degraded lint.
- **1.5.2** No new build *graph*/incremental engine — a build-routed task is one
  `cajeta <verb>` invocation; the compiler owns dependency ordering.
- **1.5.3** No removal of the "Cajeta Build" task-tree tool window — this feature
  changes where a launched build's *output* goes, not the discovery UI.

## 2. Routing policy

The classifier decides, from the discovered task + model, whether a launch is
build-routed (Build window) or run-routed (Run window). It is a pure function of
already-available data (`TaskTreeNode.Kind`, the builtin verb name,
`TaskDebugMapping.isDebuggable`).

- **2.1** As a developer, when I run a **builtin** verb that produces artifacts —
  `validate`, `compile`, `test`, `package`, `install`, `deploy` — then it runs in
  the **Build** tool window.
- **2.2** As a developer, when I run the builtin **`run`** verb, then it runs in
  the **Run** tool window (it executes my program; its stdout is program output).
- **2.3** As a developer, when I run a **user task** that is **not**
  dap-debuggable (no runnable artifact / no entry method), then it runs in the
  **Build** tool window.
- **2.4** As a developer, when I run a **user task** that **is** dap-debuggable,
  then it runs in the **Run** tool window (Run keeps its free Stop/Re-run/tabs
  for something that executes).
- **2.5** As a developer, when I **Debug** any task, then routing is unaffected —
  it uses the Debug window exactly as today.
- **2.6** The classification is centralized in one place so every entry point
  (double-click, Run Task toolbar, Run-with-args, saved config, gutter) obeys the
  same policy.

## 3. Build tool window presentation

- **3.1** As a developer, when a build-routed task starts, then a new build
  appears in the Build tool window titled `cajeta <verb>` with a running
  spinner and a Stop button.
- **3.2** As a developer, while it runs, then the compiler's stdout and stderr
  stream into that build's console incrementally (no buffering until exit).
- **3.3** As a developer, when the task exits 0, then the build finishes as
  **success** with elapsed time; on non-zero exit, as **failure**; on my
  cancelling it, as **cancelled**.
- **3.4** As a developer, when I run two build-routed tasks, then each is its own
  build node in the Build window (concurrent builds don't interleave output).
- **3.5** As a developer, when discovery/build-tool path is misconfigured
  (`BuildToolPathValidator` problem), then the build fails immediately with that
  reason as its message — no silent empty build.

## 4. Problem parsing & Problems view

The compiler's diagnostics today are **unstructured text in three observed
shapes** (verified against the built compiler). Parsing is regex over these; it
upgrades to precise ranges when `cajeta --diag-format=json` lands (constraint
§1.4.2). Because two of the three shapes **carry no file path**, navigation is
available only for the path-bearing shape — an honest limit of current output,
not of the bridge.

- **4.1** As a developer, when the compiler prints `warning: [id] msg` (direct
  cerr), then a **warning** problem node appears under the build and in the
  Problems view (positionless — the bracket is an id, not a position).
- **4.2** As a developer, when the compiler prints a semantic error in the
  `CajetaLogger` shape — a `<path>[<line>:<col>]` (or `[<line>,<col>]`) line
  followed by an `Error <id>: <msg>` line (optionally behind a glog `E…]`
  prefix) — then an **error** node appears with that message and double-clicking
  it **navigates to that file/line**.
- **4.3** As a developer, when the compiler prints an ANTLR syntax error
  `line <line>:<col> <msg>` (no path), then an **error** node appears carrying
  the line:col in its text but **without navigation** (no path to resolve) —
  it is not dropped (spec §4.4 / non-goal §1.5.1: precision awaits JSON).
- **4.4** As a developer, when a problem has no resolvable file position, then it
  still appears as a message node under the build, without navigation.
- **4.5** As a developer, when a build produces ≥1 error problem, then it also
  finishes as **failure** (§4 problems and §3.3 status agree).
- **4.6** Problem severity maps from the existing `Diagnostic.Severity`
  (`ERROR → MessageEvent.Kind.ERROR`, `WARNING → WARNING`,
  `WEAK_WARNING → WARNING`), so there is one severity vocabulary.

## 5. Lifecycle control

- **5.1** As a developer, when I press **Stop** on a running build, then the
  child `cajeta` process is force-killed and the build reports cancelled.
- **5.2** As a developer, when I use the Build window's **Restart** action, then
  the same task re-launches with the same bindings (profile/flavor/params).
- **5.3** As a developer, when I close the project with builds running, then
  their processes are terminated (no orphaned `cajeta` children).
- **5.4** Running builds are tracked so the existing tool-window **Stop active
  runs** action (`BuildRunTracker`) also stops build-routed launches, not only
  Run-routed ones.

## 6. Configuration

- **6.1** As a developer, when I want the old behaviour, then a setting
  **"Run build tasks in the Build tool window"** (default **on**) routes
  build-family tasks to Run when turned off — an escape hatch, not a new UX.
- **6.2** The setting lives with the other build-tool settings on
  `CajetaSettings` and is read in the one classification site (2.6).
