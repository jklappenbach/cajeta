# Jupyter kernel — `cajeta kernel`

## 1. Definition

**1.1 Purpose.** A `cajeta kernel` subcommand implementing the Jupyter
messaging protocol, so notebook frontends (Jupyter Lab, classic
Notebook, VS Code's notebook UI, Hex, Marimo) can drive a live cajeta
session: submit cells, keep state across them, stream output, render
results. Cells are **script units** in a multi-unit session — the
language semantics of top-level code (binding lifetime, ownership,
redefinition, diagnostics) are owned by `script-units-spec.md`; this
spec owns the kernel process: protocol, execution model, display,
interrupt, and the debugging integration promised by
`docs/specification/debugging/Debugging.md`.

**1.2 Dependency.** Script units (`script-units-spec.md`) are a hard
prerequisite: the kernel adds no cell semantics of its own. Where the
old plan wrapped cells in kernel-private synthetic classes, the kernel
now feeds each cell to the compiler as a script unit and hosts the
session scope.

**1.3 Constraints.**
- One kernel process per notebook session; cells execute one at a time
  in submission order (Jupyter's own queueing model).
- Cell execution runs off the protocol poll thread: the kernel stays
  responsive (interrupt, kernel_info, shutdown) while a cell runs.
- The kernel is the session host: it owns the persistent JIT session,
  the session scope, and the execution counter.

**1.4 Non-goals (v1).**
- No LSP (`cajeta lsp`), no time-travel debugging, no DWARF — tracked
  in `Debugging.md` separately.
- No notebook-frontend UI extensions (fiber panes, drop-chain views in
  a notebook side panel); v1 renders through standard Jupyter message
  types only.
- No multi-language cells, no magics (`%%` directives); a cell is
  cajeta source, whole.
- No hot-reload of a *separately launched* debuggee from notebook
  cells; v1 debugging scope is defined in Section 7.

## 2. Session and cell execution

Requirements:

- Each `execute_request` compiles the cell as a script unit into the
  session and runs it; the execution counter increments per executed
  cell and numbers `In[N]`/`Out[N]`.
- Cross-cell visibility, redefinition, and failure isolation follow
  script-units Section 5 exactly (last-write-wins bindings/methods,
  generational types, failed cells leave the session unchanged).
- The session persists template instantiations and compiled artifacts
  so repeated use of the same types does not recompile or duplicate
  them.

Use cases:

- **2.1** When cell 1 binds `var xs = heap ArrayList<int32>()` and
  cell 2 calls `xs.add(7)`, cell 2 compiles against the live binding
  and mutates the same value.
- **2.2** When a cell fails to compile, the reply is a structured
  error, the execution counter still advances, and the session is
  exactly as before the cell (script-units 5.5).
- **2.3** When a cell defines `class Point` and a later cell redefines
  it, the generational rules of script-units 5.3–5.4 apply; the kernel
  surfaces the "stale generation" hint verbatim in the error payload.
- **2.4** When the same stdlib specialization (`ArrayList<int32>`) is
  used in ten cells, it is compiled once for the session.
- **2.5** When a cell runs `scope { spawn … }` concurrency, carriers
  are shared session infrastructure: spawned work joins by scope rules
  within the cell, and the carrier pool survives across cells.

## 3. Protocol and lifecycle

Requirements:

- Implement Jupyter messaging v5.3 over ZeroMQ: shell, iopub, control,
  stdin (may reject input requests in v1), heartbeat; HMAC-signed
  messages per the connection file.
- `cajeta kernel --connection-file=<path>` and a bare `cajeta kernel`
  (auto-generated connection file, printed for manual attach).
- Kernel-spec installation: `cajeta init --kernel` writes the
  `kernel.json` registering `cajeta kernel` per platform.
- Graceful shutdown (`shutdown_request` and SIGTERM): the session scope
  drops (script-units 4.4), carriers shut down once, the process exits
  cleanly. Restart = shutdown + fresh session in the same process or a
  new one (frontend's choice); either way state starts empty.

Use cases:

- **3.1** When Jupyter Lab starts the kernel from its kernel spec, the
  `kernel_info_request` reply identifies the language as cajeta with
  version and file extension, and the frontend shows a live kernel.
- **3.2** When a message's HMAC does not verify, the message is ignored
  per protocol; the kernel does not crash.
- **3.3** When the frontend restarts the kernel, every session binding
  drops (destructors fire), and the next cell starts from an empty
  session with a reset execution counter.
- **3.4** When `is_complete_request` is sent mid-typing, the kernel
  answers complete / incomplete / invalid so the frontend knows whether
  to submit or continue the editing prompt.

## 4. Output, results, and errors

Requirements:

- stdout/stderr written during cell execution stream to the frontend as
  `stream` messages, in order, correlated to the requesting cell;
  output interleaves live for long-running cells (no buffering until
  completion).
- The unit result (script-units 5.6) is rendered as `execute_result`:
  primitives and `String` as text; objects through their `toString()`
  (including `@ToString` synthesis); a JSON `text/plain` +
  `application/json` dual bundle where the value's shape round-trips
  through the JSON codec. Rendering failures degrade to a type-name
  placeholder — display must never fail a successfully executed cell.
- Uncaught recoverable exceptions from a cell produce a structured
  `error` reply: exception type, message, and a stack trace rendered by
  script-units Section 6 rules (`In[N]` frames, user lines).
  Unrecoverable errors are reported as a kernel-side crash of the cell
  with as much context as can be captured; the kernel itself survives
  where the runtime allows it.

Use cases:

- **4.1** When a cell loops printing progress for 30 seconds, the
  frontend shows each line as it is written, not a burst at the end.
- **4.2** When a cell ends with `origin.x + origin.y`, `Out[N]` shows
  the numeric value.
- **4.3** When a cell ends with a `@ToString`-annotated object, its
  rendered string is the result; when the object also round-trips via
  the JSON codec, a JSON representation accompanies it for frontends
  that render structured output.
- **4.4** When a cell throws an uncaught `ProtocolException`, the
  notebook shows the type, message, and a traceback whose frames name
  cells (`In[3], line 2`) — never synthesized class names.

## 5. Interrupt and long-running cells

Requirements:

- `interrupt_request` (and SIGINT where the frontend sends it) stops
  the running cell at the next safepoint: the cell terminates with a
  KeyboardInterrupt-equivalent structured error; the session survives
  with all bindings intact; subsequent cells execute normally.
- Interrupt is best-effort at safepoint granularity: a cell blocked in
  native code without safepoints may not stop until it reaches one; the
  kernel remains responsive regardless (execution is off the poll
  thread).

Use cases:

- **5.1** When an infinite `while` loop cell is interrupted, the cell
  ends with an interrupt error within a bounded time, and the next cell
  runs against unchanged session state.
- **5.2** When interrupt arrives while no cell is running, it is a
  no-op acknowledged per protocol.

## 6. Data-science ergonomics

Requirements:

- The kernel works against the stdlib and Olla libraries exactly as any
  cajeta program: a notebook can `import` `cajeta.math`, resolve
  project dependencies (when launched inside a project directory, the
  governing `cajeta.json` classpath applies), and drive `Tensor`,
  frames, and the `dev.cajeta.ml` estimator surface cell by cell.
- Result rendering (Section 4) must be sufficient for tensors, tables,
  and estimator summaries to be legible as text in v1; richer MIME
  bundles (images, HTML tables) are staged behind display protocols in
  a later revision (open question 8.2).

Use cases:

- **6.1** When a notebook inside a project imports `dev.cajeta.ml` and
  fits a `LinearRegression` across three cells (load, fit, summary),
  each cell sees the prior cells' bindings and `summary()`'s table
  renders readably in `Out[N]`.
- **6.2** When a cell evaluates a small `Tensor`, the result rendering
  shows shape and values as text without truncating to uselessness.

## 7. Debugging integration

`Debugging.md` positions the notebook as a debugger frontend ("the
notebook IS a debug session"). This spec stages that promise:

Requirements (v1):

- Structured tracebacks and recoverable-throw interception (Section 4)
  — the notebook is a first-class place to *observe* failures.
- The kernel compiles cells with safepoints enabled (the same
  statement-boundary safepoints the DAP server uses), so pausing
  machinery has something to bind to from day one; interrupt (Section
  5) rides these safepoints.

Requirements (staged, v1.5 — tracked here, designed in `Debugging.md`):

- Jupyter debug protocol (`debug_request`/`debug_event`, the DAP-in-
  Jupyter bridge): breakpoints set on cell lines, pause/step/inspect of
  a running cell from a debug-capable frontend (VS Code notebooks,
  JupyterLab debugger panel), variable inspection via the existing DAP
  variable machinery.
- Cross-cell breakpoints per Debugging.md ("breakpoints set in one cell
  pause execution triggered from another") — a breakpoint binds to the
  session's compiled artifact, not to the cell that displayed it.

Use cases:

- **7.1** When a cell throws, the rendered traceback is enough to
  locate the failing cell and line without leaving the notebook (v1).
- **7.2** When a debug-capable frontend sets a breakpoint on a cell
  line and a later cell's call reaches it, execution pauses and the
  frontend's debugger UI shows the stack and variables (v1.5).

## 8. Open questions

- **8.1** stdin: support `input_request` (blocking reads from the
  frontend) in v1, or reject with a clear error? Recommendation:
  reject in v1; nothing in the data-science loop needs it.
- **8.2** Rich MIME display (`image/png` plots via cajeta-chart,
  `text/html` tables): which display seam do libraries target — a
  stdlib display protocol (a `Displayable` interface the kernel
  queries) or kernel-side type knowledge? Recommendation: stdlib
  protocol, specified when cajeta-chart lands; kernel ships text/JSON
  only until then.
- **8.3** Does the kernel expose `cajeta kernel --existing`-style
  attach to an already-running session (console + notebook sharing one
  session)? Recommendation: defer; single-frontend sessions in v1.
