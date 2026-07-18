# Debugging

Specification for cajeta's debugging story: the Debug Adapter
Protocol (DAP) server that makes IDE plugin authoring trivial, the
Language Server Protocol (LSP) server for editor integration, the
Jupyter-compatible kernel that turns a debugging session into a
live notebook-style interactive environment, and time-travel
debugging as a v1.5 follow-up.

Cajeta debugging is not "GDB clone." It targets two concrete user
experiences:

1. **IDE-attached debugging.** Open the project in VSCode /
   JetBrains / Helix / Zed / Emacs / vim. Plugin spawns
   `cajeta dap`; standard "set breakpoint, step, inspect" works
   from the IDE's existing UI. Plugin code is ~50 lines because
   DAP does all the work.
2. **Notebook-style live interaction.** `cajeta kernel` starts a
   Jupyter-compatible kernel. The user submits cells of cajeta
   code in a notebook frontend (Jupyter Lab, VSCode's notebook
   UI, Hex, Marimo, etc.). State persists across cells, code is
   JIT-compiled into the running kernel, breakpoints set in one
   cell pause execution triggered from another. The notebook IS
   a debug session.

Both targets share infrastructure — the JIT compiler, the RTTI
machinery, the fiber scheduler, the breakpoint engine. The IDE
debugger is one frontend; the notebook is another.

> **Status (shipped vs. design).** This document is largely a
> forward-looking specification. What exists in the compiler today:
> the `cajeta dap` subcommand (`src/main.cpp` dispatches it to
> `cajeta::dap::DapServer`); the in-process breakpoint engine
> (`src/cajeta/dbg/DebugController`); statement-boundary safepoints
> emitted under `--debug-info` / `-g` (`src/cajeta/asn/Block.cpp`,
> gated by the `CompilerMode::debugInfo` flag, off by default);
> memory-facet local metadata (`dbg::MemoryFacets`,
> `__cajeta_dbg_local`); and the destructor-breakpoint story below.
> **Not yet implemented:** `cajeta lsp`, `cajeta kernel` (the Jupyter
> kernel + hot-reload + rich-display sections), time-travel /
> `cajeta record|replay`, and **DWARF emission** — the debug-info path
> is the in-process safepoint system, *not* standard DWARF (the
> compiler has no `DIBuilder` / `DICompileUnit` codegen today). Each
> section below is shipped where it matches that list and design
> otherwise.
>
> DWARF is not planned. To debug a **compiled binary** in gdb, see
> [ExternalDebugging.md](ExternalDebugging.md): the same encoding this
> page describes is embedded in the binary and read by a gdb bridge
> (`cjbreak` / `cjstack` / `cjlocals` / `cjstep`), so one mechanism
> serves the JIT, an AOT binary, and device targets alike.

## Table of contents

1. [Goals](#goals)
2. [Non-goals (v1)](#non-goals-v1)
3. [Architecture overview](#architecture-overview)
4. [DAP server — `cajeta dap`](#dap-server--cajeta-dap)
5. [LSP server — `cajeta lsp`](#lsp-server--cajeta-lsp)
6. [Cajeta-specific DAP extensions](#cajeta-specific-dap-extensions)
7. [Kernel — `cajeta kernel`](#kernel--cajeta-kernel)
8. [Hot-reload semantics](#hot-reload-semantics)
9. [Fiber-aware debugging](#fiber-aware-debugging)
10. [Rich display protocol](#rich-display-protocol)
11. [Time-travel debugging — v1.5](#time-travel-debugging--v15)
12. [Production tracing](#production-tracing)
13. [Implementation sequence](#implementation-sequence)
14. [Open questions](#open-questions)

---

## Goals

**v1:**

- **Easy IDE plugins.** `cajeta dap` and `cajeta lsp` speak the
  standard wire protocols verbatim. Any IDE with DAP / LSP
  support gets cajeta debugging + editor features by plugging in
  a launcher. No per-IDE bespoke implementations.
- **Standard debugger primitives.** Source-level breakpoints,
  conditional breakpoints, watchpoints, step-in / step-out /
  step-over, stack inspection, variable inspection, scope
  navigation, expression evaluation in the paused frame.
- **Cajeta-specific debugger primitives.** Fiber pane (parallel
  to threads), drop-chain inspection (what runs at scope exit),
  ownership annotations in the variables panel, async-task
  tree visualization, capability-violation breakpoints, aspect-
  affected method markers.
- **Live kernel.** `cajeta kernel` over the Jupyter messaging
  protocol. Cells compile + link into a long-running JIT host;
  state persists; the kernel doubles as a debugger.
- **Hot-reload of code.** Add new top-level definitions on the
  fly. Replace method bodies in place. Existing instances keep
  working; layout-changing redefinitions are rejected with a
  clear error citing the conflict.
- **DWARF debug info on AOT binaries (planned).** The goal is for
  native debuggers (LLDB, GDB, WinDbg) to attach to a `--debug`
  binary and show source-level info from standard DWARF. **Not yet
  implemented** — the compiler currently emits in-process
  statement-boundary safepoints under `--debug-info`, not DWARF
  (no `DIBuilder` codegen). The in-process `cajeta dap` path is how
  source-level debugging works today.

**v1.5:**

- **Time-travel debugging.** Record once, replay deterministically
  with backward stepping. See [Time-travel debugging](#time-travel-debugging--v15).
- **Session record / replay in the notebook.** Each cell's input
  + state delta captured; replay or fork a session from any
  prior point.

**v2+:**

- **Production tracing** — lightweight, always-on event capture
  for diagnosing issues at scale. See [Production tracing](#production-tracing).

## Non-goals (v1)

- **Custom debugger UI.** No `cajeta debug --gui` window. IDEs do
  this better; we ride on theirs via DAP.
- **Cross-language debugging.** Cajeta debugger sees cajeta code.
  If user calls C via `@Native`, the C frames render as "native
  code (no source)" — stepping into them is delegated to a
  native debugger if attached, opaque otherwise.
- **Profile-guided debugging.** Profilers (perf, samply, Tracy
  integration) are a separate concern; the debugger doesn't try
  to be a profiler too.
- **Crash dump analysis.** Post-mortem of a core file is a
  distinct UX from interactive debugging; deferred.

---

## Architecture overview

```
                    ┌──────────────────────────────────────┐
                    │              IDE / Notebook          │
                    │   (VSCode, JetBrains, Jupyter, ...)  │
                    └─────────────┬──────────────┬─────────┘
                                  │              │
                              DAP │          Jupyter
                              LSP │      messaging (ZMQ)
                                  │              │
        ┌─────────────────────────▼──────────────▼─────────────────────┐
        │              cajeta toolchain (one binary)                     │
        │                                                                │
        │   cajeta dap        cajeta lsp        cajeta kernel            │
        │       │                 │                  │                    │
        │       └─────────┬───────┴──────────────────┘                   │
        │                 │                                              │
        │   ┌─────────────▼─────────────┐    ┌──────────────────────┐   │
        │   │     Debugger core         │    │       JIT host        │   │
        │   │  (breakpoint engine,      │◄───┤  (LLJIT, module       │   │
        │   │   stepping, scope,        │    │   linking, hot reload)│   │
        │   │   evaluator)              │    │                       │   │
        │   └─────────────┬─────────────┘    └──────────┬────────────┘   │
        │                 │                              │                │
        │   ┌─────────────▼──────────────────────────────▼────────────┐  │
        │   │              Cajeta runtime                              │  │
        │   │   (fiber scheduler, type system, RTTI, drop chains,      │  │
        │   │    capability system)                                    │  │
        │   └──────────────────────────────────────────────────────────┘  │
        │                                                                │
        └────────────────────────────────────────────────────────────────┘
```

Both the DAP server and the kernel attach to the same debugger
core + JIT host. Two front-ends of one back-end. A single user
session can have both attached simultaneously — the IDE for code
navigation + breakpoints, the notebook for exploratory cell
execution against the same paused process.

---

## DAP server — `cajeta dap`

Implements the [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/)
verbatim. Speaks JSON over stdio (or a TCP port via
`--port=<n>`). Used by every IDE that supports DAP — which is
every major IDE in 2026.

### Launching

```
cajeta dap                          # stdio mode (IDE launches the binary)
cajeta dap --port=4711              # TCP mode (IDE connects to a port)
```

### Supported DAP requests

The full standard set, plus cajeta-specific extensions described
below.

| Request                  | Purpose                                                  |
|--------------------------|----------------------------------------------------------|
| `initialize`             | Negotiate capabilities (`supportsConditionalBreakpoints`, etc.) |
| `launch`                 | Start a cajeta program under the debugger.               |
| `attach`                 | Attach to a running cajeta process (or kernel).          |
| `setBreakpoints`         | Source-line breakpoints + conditional / hit-count variants. |
| `setFunctionBreakpoints` | Break at function entry.                                 |
| `setDataBreakpoints`     | Watchpoints — break on read / write to a field.          |
| `setExceptionBreakpoints`| Break on thrown exceptions, optionally filtered by type. |
| `continue` / `pause`     | Standard run / pause.                                    |
| `next` / `stepIn` / `stepOut` | Single-step over / into / out of a call.            |
| `stackTrace`             | Current call stack.                                      |
| `scopes` / `variables`   | Scope hierarchy, variable values in scope.               |
| `evaluate`               | Evaluate an expression in a paused frame.                |
| `setVariable`            | Modify a variable's value in a paused frame.             |
| `threads`                | List of threads (in cajeta: list of fibers — see below). |
| `disassemble`            | Show generated assembly for a region of code.            |
| `source`                 | Resolve a source-location reference.                     |

### Launch configuration shape

What IDE plugins send to the DAP server when starting a debug
session:

```jsonc
{
    "type":         "cajeta",
    "request":      "launch",
    "name":         "Debug my-service",
    "manifest":     "${workspaceFolder}/cajeta.json",
    "flavor":       "debug",
    "entry-method": "com.example.Main::main",
    "args":         ["--port=8080"],
    "env":          { "LOG_LEVEL": "trace" },
    "cwd":          "${workspaceFolder}",
    "stopOnEntry":  false,
    "sourceMaps":   "auto"
}
```

The `type: "cajeta"` selector tells the IDE which DAP server to
launch. The plugin's contribution to the IDE is essentially:

```jsonc
{
    "languages":  [{ "id": "cajeta", "extensions": [".cajeta"] }],
    "debuggers": [{
        "type":        "cajeta",
        "label":       "Cajeta",
        "program":     "cajeta",
        "args":        ["dap"],
        "languages":   ["cajeta"]
    }]
}
```

That's it. The full DAP standard does the rest.

---

## LSP server — `cajeta lsp`

Implements the [Language Server Protocol](https://microsoft.github.io/language-server-protocol/)
for editor features: autocomplete, hover, go-to-definition,
find-references, rename, diagnostics, formatting.

```
cajeta lsp                          # stdio mode
cajeta lsp --port=2087              # TCP mode
```

### Supported LSP capabilities

| Request                  | Purpose                                                |
|--------------------------|--------------------------------------------------------|
| `textDocument/completion`| Auto-complete identifiers + types + keywords.          |
| `textDocument/hover`     | Doc-comment popup on hover.                            |
| `textDocument/definition`| Jump to definition.                                    |
| `textDocument/references`| Find all uses.                                         |
| `textDocument/rename`    | Rename across the project, capability-aware.           |
| `textDocument/codeAction`| Quick fixes (add missing capability, fix import, ...). |
| `textDocument/formatting`| `cajeta fmt`-equivalent on save.                       |
| `textDocument/diagnostics`| Errors + warnings inline.                              |
| `textDocument/inlayHint` | Type-argument hints, ownership annotations inline.     |
| `textDocument/codeLens`  | Above-method "Run | Debug | Doc" buttons.              |
| `workspace/symbol`       | Project-wide symbol search.                            |

### Cajeta-specific LSP behaviors

- **Capability-aware completion.** If `cajeta.json` doesn't
  declare `network`, network APIs (`cajeta.io.net.Socket`,
  `cajeta.io.net.http.HttpClient`) don't appear in completion
  suggestions — and using them gets a diagnostic with a code
  action to add the capability.
- **Aspect-affected markers.** Methods wrapped by `@Around` /
  `@Before` advice get a CodeLens annotation: "wrapped by 2
  aspects" with a link to the advice methods.
- **Ownership annotations as inlay hints.** A call site
  `process(data)` renders as `process(data: <#borrowed>)` or
  `process(#data: <#owned>)` depending on the parameter's
  ownership semantics. Reduces ambient memory-model surprise.
- **Type-argument inlay hints.** `Box<int32>` is shown at use
  sites even when type inference picks it: `var x = heap Box(42)`
  becomes `var x: <Box<int32>> = heap Box(42)`.

---

## Cajeta-specific DAP extensions

DAP allows custom requests for protocol additions. Cajeta's
debugger ships these — IDE plugins that want to render them
specially can; others see them as opaque structured data.

### Fiber pane

The standard DAP `threads` request returns OS threads. Cajeta's
runtime schedules fibers across a small thread pool, so the
threads view alone misses 90% of what the user wants to inspect.
Custom request:

```jsonc
// Request
{ "command": "cajeta:fibers" }

// Response
{
    "fibers": [
        {
            "id":       42,
            "name":     "request-handler-fiber",
            "state":    "running" | "ready" | "parked" | "waiting-io",
            "carrier":  "thread-0",
            "stackTop": { "source": "Main.cajeta", "line": 142 }
        },
        ...
    ]
}
```

IDE plugins render this as a "Fibers" tab alongside Threads.
Clicking a fiber selects it as the current debug target;
`stackTrace` and `variables` then reflect that fiber's state.

### Capability pane

```jsonc
{ "command": "cajeta:capabilities" }

// Response
{
    "declared": ["network", "filesystem", "env", "clock", "random"],
    "used":     ["network", "filesystem", "env", "clock", "random"],
    "violations": []
}
```

For the current process, shows declared vs actually-exercised
capabilities. Useful for trimming over-declared sets during
development.

### Drop chain inspection

When paused, lists the pending drops in the current scope —
what will fire when the function returns or scope exits.

```jsonc
{ "command": "cajeta:dropChain" }

// Response
{
    "scope": "function:fetchUrl",
    "drops": [
        { "value": "tempBuffer",  "type": "int8[]",       "dropFn": "__cajeta_free_array" },
        { "value": "conn",        "type": "TcpConnection","dropFn": "TcpConnection.drop"   }
    ]
}
```

### Drop / destructor breakpoints

To answer *"when was this instance destructed?"*, set an **ordinary source
breakpoint on the class's destructor** — `~T()`. No special breakpoint type and
no protocol extension are involved:

- Every class implicitly extends `Object`, which declares the root virtual
  destructor `~Object()`. Destruction is virtual — dropping through any base
  reference dispatches to the most-derived `~T()` via the vtable drop slot.
- A `~T()` body is a normal method body, so under `--debug-info` it carries the
  same per-statement safepoints as any other code. A breakpoint on a line in
  `~T()` therefore parks through the existing DebugController rendezvous when an
  instance of `T` is dropped (at scope exit, when a binding is reassigned, or
  when an owner is set to `null` — cajeta has no `delete`/`free`; reclamation is
  the scope-exit drop chain).
- It composes with conditional breakpoints (CP6f-1) and is toggled/removed live
  like any line breakpoint.

To make a class's destruction observable, give it a `~T()` (even a trivial one)
and breakpoint its body. A class with no `~T()` runs only `Object`'s empty
destructor plus the synthesized field-drop/free wrapper, which carry no
user-visible source line — add a `~T()` if you need to stop there.

### Ownership annotations in variables

The standard DAP `variables` response gets a namespaced `cajeta`
extension field per variable carrying three orthogonal memory
facets (CP7-1d; see `ide-plugins/idea/ide-plugin-debug-fr-1.md`):

```jsonc
{
    "name":      "data",
    "value":     "<int8[1024]>",
    "type":      "int8[]",
    "variablesReference": 17,
    "cajeta": {
        // where the value lives
        "alloc":     "stack" | "heap" | "shared" | "unknown",
        // who is responsible for it
        "ownership": "owner" | "borrow" | "moved"  | "unknown",
        // lifetime state at this stop
        "lifetime":  "live"  | "moved-out" | "about-to-drop" | "unknown"
    },
    // standard DAP hint: a moved-out (consumed) binding is read-only
    "presentationHint": { "attributes": ["readOnly"] }
}
```

The three axes are independent (a value can be heap + owner +
about-to-drop, or heap + borrow + live). Each tag is always
present — `"unknown"` is emitted explicitly rather than omitted so
the plugin renders a neutral state instead of guessing. The tags
are the authoritative, color-independent carrier; the plugin maps
them to icon + color + treatment. `presentationHint.attributes`
gets `"readOnly"` for a moved-out binding so a generic DAP client
also blocks editing a consumed value.

The facets originate in the compiler (`dbg::MemoryFacets`),
travel through the runtime debug frame chain (`__cajeta_dbg_local`
carries the alloc/ownership bytes + the owner's drop-entry
pointer), and `lifetime` is derived host-side at the stop from the
drop entry's live `active` flag.

### Async task tree

```jsonc
{ "command": "cajeta:asyncTasks" }

// Response
{
    "tasks": [
        {
            "id":     1,
            "name":   "main-task",
            "state":  "running",
            "children": [
                { "id": 2, "name": "fetch(api/users)",  "state": "awaiting",  "awaitedOn": "io" },
                { "id": 3, "name": "fetch(api/orders)", "state": "completed" }
            ]
        }
    ]
}
```

Parent-child task hierarchy with await points and resolution
states. Useful for understanding why a task is stuck waiting.

### Capability-violation breakpoints

```jsonc
{
    "command": "cajeta:setCapabilityBreakpoint",
    "arguments": { "capability": "network", "break": true }
}
```

Triggers a breakpoint the next time the program uses the named
capability. Useful for "where exactly does this thing hit the
network?" investigations.

---

## Kernel — `cajeta kernel`

Implements the [Jupyter messaging protocol](https://jupyter-client.readthedocs.io/en/latest/messaging.html)
— a ZeroMQ-based wire format used by Jupyter Lab, classic Jupyter
Notebook, VSCode's notebook UI, Hex, Marimo, and other notebook
frontends.

### Launching

```
cajeta kernel                                    # auto-generated connection file
cajeta kernel --connection-file=path.json        # explicit connection params
```

Frontends typically don't launch the kernel directly — they go
through a "kernel spec" registration:

```jsonc
// ~/.local/share/jupyter/kernels/cajeta/kernel.json
{
    "argv":         ["cajeta", "kernel", "--connection-file={connection_file}"],
    "display_name": "Cajeta",
    "language":     "cajeta"
}
```

`cajeta init --kernel` installs this kernel.json into the right
place per platform.

### Kernel state model

The kernel maintains:

- A **persistent JIT host** (`LLJIT` instance) accumulating
  compiled modules across cells.
- A **persistent symbol table** mapping top-level names (variables,
  functions, classes) to their compiled artifacts. New cells see
  prior cells' definitions.
- A **persistent heap** — values bound to top-level names stay
  alive across cells.
- **Capture buffers** for stdout / stderr — output streams back to
  the frontend as `stream` messages.
- An **execution counter** incremented per cell.

When the frontend sends an `execute_request` with cell source:

1. Parse the cell as cajeta source (treated as if it were a tiny
   anonymous module).
2. Resolve types against the persistent symbol table.
3. Compile to LLVM IR.
4. Link the new IR into the running JIT.
5. Execute. Top-level expressions return values; the kernel
   formats and sends the value as `execute_result`.
6. Bind new top-level names into the persistent symbol table.

The execution counter is the `In[N]:` / `Out[N]:` numbering
notebooks use.

### Cell evaluation rules

A cell is either:

- **A statement sequence** — like a function body. Top-level
  variable declarations bind in the symbol table; expression
  statements evaluate for side-effects.
- **A single expression** — evaluated and returned as
  `execute_result`. Notebooks render this as `Out[N]:`.
- **A declaration** — `public class Foo { ... }`, `public static
  int32 fn() { ... }` — added to the symbol table; no `Out[N]`
  output unless the cell ends with an expression.

Cells mix all three:

```cajeta
// Cell 5:
public class Point {
    public int32 x;
    public int32 y;
    public Point(int32 x, int32 y) { this.x = x; this.y = y; }
}

Point origin = heap Point(0, 0);
origin.x + origin.y
```

This declares `Point`, binds `origin` into the symbol table,
returns `0` as `Out[5]:`.

### Notebook + debugger combined

The kernel and the debugger share a process. When a cell hits a
breakpoint:

1. The fiber executing the cell parks at the breakpoint.
2. The kernel notifies the frontend via Jupyter's `debug_event`
   message + cajeta-specific extension data (the fiber id, the
   stack, the local variables).
3. Frontend renders the paused state — VSCode shows it in the
   debugger panel; standalone notebook frontends render it in a
   side panel via cajeta-specific frontend extensions.
4. User can submit new cells while paused. Cells execute in the
   paused fiber's scope — declared variables visible, helper
   functions callable, modifications to mutable state stick.
5. User resumes; the original cell continues.

This is the killer feature. Defining a debugging helper function
mid-investigation and immediately invoking it against the paused
state — Smalltalk / Lisp / Clojure productivity, applied to a
statically-typed compiled language via the JIT.

---

## Hot-reload semantics

The kernel accepts incremental code submissions. Three categories
of redefinition, with three different policies:

### 1. New top-level definitions — always allowed

Declaring a new type, function, or variable that doesn't conflict
with an existing name. Trivially supported. The JIT compiles +
links + symbol table updates.

```cajeta
// Cell 3:
public class Cache { ... }
// → registered as `Cache` in the symbol table
```

### 2. Body-only changes to existing definitions — allowed

Replacing a method's implementation without changing its
signature, or replacing a top-level function's body. The JIT
generates a new function, swaps the symbol table entry, and
existing call sites resolve to the new version on next dispatch.

```cajeta
// Cell 7:
public static int32 score(int32 raw) {
    return raw * 2;
}

// Cell 12 — replaces score's body:
public static int32 score(int32 raw) {
    return raw * 3 + 1;
}
```

Caveat: in-flight calls don't pick up the new body. They finish
with the old code. The next call dispatches to the new code.

### 3. Layout-changing redefinitions — refused with a clear error

Adding / removing / reordering fields on a class with live
instances, changing a method's signature, changing inheritance —
any redefinition that would invalidate existing instances'
memory layout or existing call sites' assumptions is rejected:

```
Cell 19: error: cannot redefine `Point` — field layout would change
  declaring `int64 z` would shift the layout of existing instances
  (3 instances of Point currently allocated on the heap)

  options:
    1. Define a new type `Point3D` with the additional field
    2. Restart the kernel to discard existing instances
    3. Migrate manually: `for (p in Cajeta.heap.instancesOf(Point)) { ... }`
```

This refuses-rather-than-corrupts policy is the simplest correct
model. Python / IPython chose append-only (shadow the old
definition; old instances keep working with old behavior); the
surprise that "I redefined my class but old instances behave
strangely" makes that the wrong choice for a statically-typed
language with explicit memory layout. Cajeta prefers honesty.

Future work — **migration mode** (option 3 above) is a v2
feature: walk the heap finding instances of the old type,
project their fields onto the new layout where feasible, free
the old. Requires more reflective machinery than v1 ships and
genuinely-conflicting layout changes have no migration; the
honest "refused" error is right for v1.

---

## Fiber-aware debugging

Cajeta's runtime is fiber-scheduled. The OS sees a small thread
pool; the runtime multiplexes many fibers across it. Pausing
"the program" doesn't mean what it does in a thread-per-request
system.

### Pause modes

Two modes, switchable per-debug-session:

**Pause-everything (default).** When a breakpoint fires, every
fiber freezes. Carrier threads stop scheduling. The debugger sees
a fully stopped world. Easy to reason about; right for typical
"is this one bug producing wrong output?" investigations.

**Single-fiber-pause (opt-in).** Only the fiber that hit the
breakpoint freezes. Other fibers continue scheduling. The
debugger can inspect the paused fiber while production-ish
activity continues. Right for "what's wrong with this one
connection out of 100k?" investigations on long-running servers.

Switched via DAP customRequest:

```jsonc
{ "command": "cajeta:setPauseMode", "arguments": { "mode": "single-fiber" } }
```

### Fiber as DAP "thread"

The standard DAP `threads` response lists OS threads. Cajeta
maps **fibers** to the DAP thread abstraction by default
(configurable via `cajeta:setThreadsView=fibers|os|both`). Most
IDE plugins render the "Threads" tab; mapping fibers there gives
users the natural unit of concurrent work without the IDE plugin
needing custom UI.

Pure OS-thread view is available for users who want to see
scheduler carriers (e.g. confirming work distribution across
cores). The `both` view shows a tree — carrier threads as parents,
the fibers they're currently running as children.

### Async-task stepping

When stepping into an `await` point on a `Task<T>`:

- **Step Over** treats the entire await as a single line — the
  fiber blocks on the task's completion, the debugger waits, then
  continues past the await.
- **Step Into** descends into the awaited task's body. The
  debugger switches to whatever fiber is executing that task and
  steps from there.

Either mode preserves the calling fiber's stack as the "parent
frame" so `stepOut` returns to the await site.

---

## Rich display protocol

Jupyter supports MIME-typed outputs: text, HTML, images, SVG,
LaTeX, structured tables, plotly / vega plots, ipywidgets. Cajeta
types declare their representations:

```cajeta
public class HeatMap implements Displayable {
    private float64[][] data;
    private String title;

    public Display[] displayHints() {
        return [
            Display.png(renderToPng()),         // image/png
            Display.html(renderToHtml()),       // text/html — table view
            Display.plain(toString())            // text/plain — fallback
        ];
    }
}
```

The frontend picks the richest MIME type it can render. Plain-text
notebook frontends get `text/plain`; web-based frontends get the
PNG or HTML.

Stdlib types ship sensible defaults:

- `cajeta.math.tensor.Tensor` — text repr for small shapes,
  HTML table or PNG heatmap for larger 2D / 3D shapes.
- `cajeta.io.Buffer` — hex dump up to a configurable cap.
- `Array<T>` — bracketed comma list for small arrays, truncated
  with `... <N more>` for large.
- All other types — `toString()` fallback.

Annotations as shortcut:

```cajeta
@DisplayAs(mime = "image/png", method = "renderToPng")
public class Chart { ... }
```

The compiler synthesizes a `displayHints()` returning the
annotation's MIME + method.

---

## Time-travel debugging — v1.5

Mozilla's [rr](https://rr-project.org/) approach, adapted for
cajeta. Record once, replay deterministically, step backward
through the recorded execution.

### Why it matters

The classic frustrating case: a value is wrong when you notice
it, but the corruption happened 30 seconds and ten thousand
function calls earlier. Conventional debugging: set hypothetical
breakpoints, re-run, hope you guessed right, repeat. Time-travel
debugging: breakpoint *after* the symptom, hit it, step backward
through execution until you find where the value got wrong. The
bug location reveals itself directly.

### The recording phase

Every source of non-determinism in cajeta is intercepted and
recorded. Cajeta is a uniquely good host for this because we
control the runtime:

| Non-determinism source                    | Recording strategy                                            |
|-------------------------------------------|---------------------------------------------------------------|
| `System.currentTimeMillis()` / `Instant.now()` | Record returned timestamp                                |
| `Math.random()`, `SecureRandom`           | Record returned bytes / values                                |
| Per-process hash seed                     | Record at startup                                             |
| `/dev/urandom` reads                      | Record returned bytes                                          |
| Filesystem reads / writes                 | Record returned bytes + syscall results                       |
| Network reads / writes                    | Record returned bytes + syscall results                       |
| Fiber scheduler decisions                 | Record which fiber the carrier picks at each yield point      |
| Signal delivery                           | Record signal + instruction-count point                       |
| `clock_gettime` / `getuid` / `getenv`     | Record returned value                                         |
| `pthread_*` / cond / mutex outcomes       | Record acquisition order + return values                      |

Recording overhead estimate: ~1.5-2× the program's runtime
(matches rr's overhead on similar instrumentation density).
Storage: ~50-200 MB per minute of recording for typical
workloads, compressed via zstd as it's written.

### The replay phase

Same binary, recorded inputs replayed at the same
instruction-count points. Deterministic — every instruction
executes in the same order, with the same inputs as the original
run.

The debugger runs against the replaying process. Forward-stepping
works normally. Backward-stepping uses periodic state snapshots:

- The recorder takes a memory snapshot every N instructions
  (default: every 100 milliseconds of recording time, configurable).
- "Step backward one instruction" → rewind to nearest prior
  snapshot, replay forward to instruction-1.
- "Reverse-continue until breakpoint X" → similar but scans
  forward looking for the breakpoint condition.

### DAP integration

DAP standardizes `reverse-continue` and `stepBack` for time-travel
debuggers. The cajeta DAP server exposes them when the session
is replaying a recording.

```jsonc
// Request
{ "command": "stepBack",  "arguments": { "threadId": 42 } }
{ "command": "reverseContinue", "arguments": { "threadId": 42 } }

// Plus cajeta-specific:
{ "command": "cajeta:jumpToTime",
  "arguments": { "wallTimeMs": 12345678 } }

{ "command": "cajeta:jumpToCellResult",
  "arguments": { "kernelCell": 7 } }
```

`jumpToTime` is the killer use case for production crash analysis
— a stack trace from a crash names the wall-clock time; the
debugger can rewind directly to that moment.

`jumpToCellResult` is the notebook integration — fork the session
back to the state after cell N completed, re-run cells N+1
onward with different code.

### CLI

```
cajeta record <binary> [args]      # run with recording, save trace to ./recordings/
cajeta replay <trace>              # open trace in the debugger
cajeta replay <trace> --port=4711  # replay with DAP server listening
cajeta record --list                # list recordings in the project
cajeta record --prune <selector>   # delete older recordings
```

Recordings are stored in `.cajeta/recordings/<timestamp>-<binary>/`,
gitignored by default. Each recording is a directory with metadata
+ event log + periodic snapshots. Average size ~100 MB per recorded
minute after zstd compression.

### Platform support

| Platform        | v1.5 support  | Notes                                                  |
|-----------------|---------------|--------------------------------------------------------|
| Linux x86_64    | Yes           | Uses ptrace + perf_event_open + seccomp-bpf. Matches rr's approach. |
| Linux aarch64   | Yes           | Same techniques.                                       |
| macOS           | Tier 2 (v2)   | Requires Mach-task-port machinations + System Integrity Protection workarounds. Doable but more work than v1.5 scope. |
| Windows         | Tier 2 (v2)   | Possible via TTD (Time Travel Debugging) framework; v2 effort. |
| wasm32-wasi     | No            | WebAssembly's execution model precludes the recording techniques used here. Out of scope. |

### Cost of recording opt-in

Recording isn't always-on — `cajeta build --record-ready` produces
a binary that supports recording (links the recorder hooks); a
plain `cajeta build` produces a binary that doesn't. The runtime
cost of always-on hooks is too high for production builds; the
opt-in keeps the cost paid only by development / test runs.

The kernel always runs in `--record-ready` mode so notebook
sessions can be recorded for replay later.

### Session record / replay in the notebook (the v1.5 minimum)

Even before full instruction-level time travel, the kernel
supports a lightweight "session record":

- Every cell's input source + the kernel's state delta (new
  bindings, mutated values) get logged.
- `cajeta kernel --replay=<session-file>` re-executes the cells in
  order, restoring state at each step.
- `cajeta kernel --fork=<session-file>:<cell>` starts a kernel
  with state restored to the moment after cell `<cell>` completed
  — user can then submit new cells from that branch point.

This is ~1% the implementation effort of full rr-style time
travel and 80% the value for the interactive-development
workflow. Ships in v1; full time travel lands in v1.5.

---

## Production tracing

A separate user experience from interactive debugging.
Always-on, low-overhead structured event capture. Useful when
the symptom shows up in production logs but reproducing locally
fails.

Scope hints:

- `@trace` annotation on functions — every call records entry +
  exit + arguments + return value to a circular buffer in
  memory. Negligible overhead when not actively read.
- Capture buffer dumped on crash, signal, or external request.
- Standard OpenTelemetry export so existing tooling
  (Honeycomb / Jaeger / Datadog) can consume it.
- Time-travel can replay from a captured production trace if
  the binary was built `--record-ready` and the trace included
  enough non-determinism.

Detailed design for production tracing lands in a separate spec
(`Tracing.md`) once the v1 debugger is solid. Out of scope here.

---

## Implementation sequence

A reasonable order, building on the existing JIT infrastructure:

**v1 (basic debugger):**

1. **DAP server skeleton.** Implement the protocol envelope —
   message framing, JSON marshaling, the `initialize` /
   `launch` / `disconnect` lifecycle. Stub the inspection
   requests.
2. **Breakpoint engine.** Source-line breakpoints via instruction-
   pointer instrumentation in JIT'd code. AOT debug builds use
   DWARF + standard ptrace integration. Conditional breakpoints
   via compiled expression evaluators.
3. **Step / pause / continue.** Single-step (instruction or
   line), step-in, step-out, step-over. Pause-everything mode
   first; single-fiber-pause as a v1.1 follow-up.
4. **Stack + variable inspection.** Walk the call stack, resolve
   variable values per scope. Cajeta's RTTI + structural type
   info drives the rendered representations.
5. **Expression evaluator.** Evaluate cajeta expressions in a
   paused frame. Reuses the parser + JIT path that already
   compiles user code.
6. **LSP server.** Completion / hover / definition / references /
   rename / diagnostics / formatting. Shares the parser with
   the compiler.
7. **Cajeta-specific DAP extensions.** Fibers, capabilities,
   drop chains, ownership annotations, async task tree.
8. **Kernel skeleton.** Jupyter messaging protocol over ZMQ.
   Cell execution against the JIT. Stdout / stderr capture.
   Display protocol for cajeta's `Displayable` types.
9. **Hot reload.** New definitions, body-only changes, layout-
   change refusal with clear errors.
10. **DWARF emission in debug builds.** Cajeta source line +
    column → LLVM IR debug location → DWARF in the binary. AOT
    debugging via LLDB / GDB works.
11. **Kernel-debugger integration.** Breakpoints set in cells
    pause execution; new cells can be submitted while paused;
    execution resumes from the original cell's site.

**v1.5 (time travel):**

12. **Session record / replay in the kernel.** Cell-level
    granularity; the lightweight starter form. Ships as soon
    as the kernel is solid.
13. **Recording infrastructure for AOT binaries.** Linux x86_64
    first. Recorder hooks for each non-determinism source.
14. **Replay engine.** Deterministic re-execution with periodic
    state snapshots.
15. **Backward stepping.** Snapshot-based rewind + replay-forward
    to target. DAP `stepBack` / `reverseContinue` support.
16. **Linux aarch64 recording.** Once x86_64 is solid.

**v2+ (deferred):**

17. **macOS time travel.** Mach-task-port integration; SIP
    workarounds.
18. **Windows time travel.** TTD framework or equivalent.
19. **Production tracing** — separate spec (`Tracing.md`).
20. **Migration-mode hot reload** — walk the heap, project
    instances of old types onto new layouts where feasible.

---

## Open questions

- **Notebook frontend ecosystem.** Jupyter is the lingua franca,
  but the next generation (Marimo, Hex, Observable) all have
  somewhat different feature sets and stability guarantees. We
  target Jupyter compatibility as the baseline; cajeta-specific
  notebook UI features (the in-line debugger panel, fiber pane,
  capability viewer) need cajeta-specific frontend extensions
  per platform. Lean: ship the kernel against Jupyter's protocol
  exactly; extension modules per frontend follow demand.
- **Recording storage policy.** How long to keep recordings,
  whether to auto-prune by age / size / count. Lean: default to
  size-based with a configurable cap (e.g. 10GB across all
  recordings); LRU eviction.
- **Replay-from-production.** Can a recording captured in
  production be replayed locally? Only if (a) the binary is
  the same, (b) the recording captured enough non-determinism,
  (c) shared-memory regions are captured. (c) is hardest —
  cajeta programs that mmap shared memory may not replay
  cleanly. Lean: document as "best effort"; programs that need
  guarantees use the simpler logging-based approach.
- **Live-code-reload limits.** v1's "refuse incompatible
  redefinitions" is the safe choice but frustrates users who
  want Python-style flexibility. The v2 migration-mode is a
  real fix but a big implementation. Worth surveying users
  after v1 ships to see how often they hit the limit.
- **Performance overhead of fiber-aware DAP.** Walking the
  fiber list, building the async-task tree, computing per-fiber
  stacks at each pause is O(active fibers). Programs with
  100k+ live fibers (long-poll servers, IoT gateways) may
  experience long pause times when the debugger attaches. Lean:
  lazy-compute these views; the IDE expands them on demand
  rather than pre-fetching for every pause.
- **DAP capability negotiation drift.** The DAP standard
  evolves; new requests get added over time. We pin to a
  version of the spec for v1; matching the latest spec is a
  rolling effort. Lean: pin to DAP 1.65 (the current as of
  v1 design) and document the version explicitly.
