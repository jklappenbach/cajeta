# cajeta.process — Specification

> Status: **draft, pending approval.** Authored with the **design** skill. Branch:
> `feature/mcd-lift`. A new **standard-library** package (`runtime/src/cajeta/process/`)
> giving cajeta programs OS subprocess control — spawn a child, wire its stdio, feed it
> input, capture output, wait for exit, and kill/timeout it. Backed by new
> compiler-intrinsic runtime bridges (`__cajeta_proc_*` C functions in
> `runtime/native/cajeta_runtime.c`, intrinsic-lowered at the call site like
> `cajeta.io.file`), mirroring the semantics of the already-proven, cross-platform
> host-side `Subprocess` facility (`src/cajeta/buildtool/Subprocess.h`).

## 1. Definition

### 1.1 Purpose
A standard-library subprocess subsystem so cajeta **programs** (not just the C++ toolchain)
can run external commands: build CLI tools, orchestrators, test harnesses, and agent-facing
services that shell out. It is the missing OS-integration primitive between cajeta and the
process tree — the analog, in the target language, of the host build tool's `runSubprocess`.

### 1.2 Why
cajeta already has the *data* primitives a tool needs (JSON/YAML, base64, sha256, clock,
collections, file I/O, env via `System.env`, argv via `main(String[])`) and stdio is
reachable through `File(fd)`. The one missing piece for writing real tooling **in cajeta**
is spawning and controlling child processes — there is no `fork`/`exec`/`spawn`/`waitpid`
bridge in the runtime today (the only process-adjacent bridges are `__cajeta_args_make`,
`__cajeta_env_get`, `__cajeta_env_set`). This package fills that gap. Its first concrete
consumer is a cajeta-written `cajeta-mcp` server (`docs/specs/cajeta-mcp-spec.md`), which
must spawn `cajeta compile`/`jit-run`/`search-skill` children and run each `jit_execute` in
a fresh child (process-per-execute) — but the capability is general-purpose.

### 1.3 Scope
- A `cajeta.process` package: a `Command` builder, a `Process` handle, a `ProcessResult`
  value, and a `Stdio` wiring mode, over new `__cajeta_proc_*` runtime bridges.
- **One-shot** execution: build a command, run it to completion, get exit status +
  captured stdout/stderr.
- **Streaming / long-lived** execution: spawn a child whose stdin/stdout/stderr are live
  `File`-backed handles the program reads/writes incrementally, plus `wait`, wait-with-
  timeout, and `kill`.
- Command inputs mirroring the proven host model: `argv` (program + args, no shell),
  working directory, environment (inherit or replace), stdin bytes.
- Cross-platform: POSIX (`posix_spawn`/`fork`+`execvp`+`waitpid`) and Windows
  (`CreateProcess`), behind one bridge seam.

### 1.4 Non-goals
- **Shell interpretation.** Commands are an explicit `argv` vector; no `/bin/sh -c`
  string parsing, globbing, or pipe-syntax. (A caller may still spawn a shell explicitly.)
- **Pipelines / job control / PTYs.** No `a | b` orchestration primitive, no terminal
  control, no background-job table — v1 is single-child spawn + wait.
- **Async-reactor integration.** v1 calls are blocking (fiber-friendly, §6); wiring child
  I/O into `cajeta.io.net`'s async reactor is future work.
- **Capability-policy enforcement.** Spawning is a privileged capability; surfacing it in
  the manifest capabilities model / sandbox is noted (§7.3) but not enforced here in v1.
- **Daemonization / process groups / setuid** and other deployment-shaped controls.

## 2. Command construction

### 2.1 Requirements
- `Command` is built from a non-empty `argv` (`argv[0]` is the program, resolved against
  `PATH` when it is not already a path to an existing file), with optional, chainable
  configuration: working directory; environment (default = inherit parent; or a full
  replacement set; or parent-plus-overrides); and stdin bytes.
- A `Command` is reusable: building it performs no OS work; `run()`/`spawn()` (§4/§5) is
  what touches the OS. The same `Command` may be run more than once.
- An empty `argv` is a construction-time error.

### 2.2 Use cases
- **2.2.1** As a program, I build `Command(["cajeta", "search-skill", q])` and run it.
- **2.2.2** As a program, I set a working directory so the child resolves relative paths
  against the project root, not the parent's cwd.
- **2.2.3** As a program, I add an environment override (e.g. `CAJETA_SOURCE_ROOT=…`)
  on top of the inherited environment.
- **2.2.4** As a program, I provide stdin bytes (e.g. a JSON-RPC request) that the child
  reads to completion.

## 3. Stdio wiring

### 3.1 Requirements
- Each of the child's three standard streams is independently wired by a `Stdio` mode:
  **Inherit** (share the parent's fd), **Capture** (collect all bytes, returned in the
  result), **Pipe** (a live `File`-backed handle for incremental read/write), or **Null**
  (discard / empty).
- The default wiring matches the proven host model: stdin Inherit (unless stdin bytes are
  supplied, which implies a closed-after-write pipe), stdout/stderr Inherit unless a
  capture/pipe is requested.
- **Pipe** handles reuse the existing `File`/`FileReader`/`FileWriter` surface over the
  pipe fds (`File(fd)` already exists) — no new stream type is introduced.
- Capturing both stdout and stderr concurrently must not deadlock on a child that fills
  both pipe buffers (the bridge drains them without blocking the child).

### 3.2 Use cases
- **3.2.1** As a program, I Capture a child's stdout and read its full output from the
  result after it exits.
- **3.2.2** As a program, I let a child Inherit my stdout so its output passes straight to
  my terminal.
- **3.2.3** As a program running a long-lived child, I get its stdout as a `FileReader` and
  read framed messages incrementally while it runs (§5).
- **3.2.4** As a program, a child that writes megabytes to both stdout and stderr is fully
  captured without deadlock.

## 4. One-shot execution

### 4.1 Requirements
- `Command.run()` spawns the child, performs the configured stdio (writing any stdin bytes,
  draining captures), waits for exit, and returns a `ProcessResult`.
- `ProcessResult` reports, mirroring the host model: whether it **launched**; whether it
  **exited** normally and its **exit code**; whether it was **signaled** and the **signal**
  (POSIX; always false on Windows); a normalized `code()` (exit code on normal exit,
  `128+signal` when signal-killed, `-1` otherwise); captured **stdout**/**stderr** when
  those streams were Captured; and whether it **timed out** (§5.3).
- A **non-zero exit is a normal result**, not an error — the program inspects `code()`.
- A **failure to launch** (e.g. program not found) is surfaced as `launched == false` with
  `code() == -1` — a result, never a throw (§8.2). The caller checks `launched()`.

### 4.2 Use cases
- **4.2.1** As a program, `Command([...]).run()` of a program that exits 0 returns
  `launched=true, exited=true, code()==0`.
- **4.2.2** As a program, a child that exits non-zero returns `code()==N` and is not raised
  as an error.
- **4.2.3** As a program, running a non-existent program reports a launch failure distinct
  from any exit code.
- **4.2.4** As a program, I run with captured stdout and read the exact bytes the child
  produced from the result.

## 5. Streaming / long-lived execution

### 5.1 Requirements
- `Command.spawn()` starts the child and returns a `Process` handle **without** waiting.
  When a stream is wired Pipe, the handle exposes it as a `File`-backed reader/writer
  (`stdin()`/`stdout()`/`stderr()`).
- `Process` supports: `wait()` → `ProcessResult` (block until exit), `wait(Duration)` →
  optional result (block up to a timeout), `kill()` (terminate; POSIX SIGKILL / Windows
  TerminateProcess), and querying the live `pid`.
- Closing the handle's stdin writer signals EOF to the child; dropping the `Process`
  without waiting does not leak a zombie (the bridge reaps it).

### 5.2 Use cases
- **5.2.1** As a program, I `spawn()` a child, write a request to its stdin pipe, read its
  response from its stdout pipe, then `wait()` for clean exit.
- **5.2.2** As a program, I `wait(Duration)` and, on timeout, `kill()` the child; the next
  `wait()` reports it as signaled / timed-out.
- **5.2.3** As a program, a child I `spawn()` and never `wait()` for is still reaped (no
  zombie) when its `Process` is dropped.

### 5.3 Timeout & termination
- Both `run()` (with an optional timeout) and `Process.wait(Duration)` enforce a deadline;
  on expiry the child is killed and the result is flagged `timedOut`. This is the safety
  valve a server uses so submitted code that hangs is reaped, not waited on forever.

## 6. Execution / blocking model

### 6.1 Requirements
- v1 calls are **blocking**, but fiber-friendly: a `wait()`/`run()` that blocks blocks the
  calling **fiber**, yielding its carrier thread to other fibers (consistent with how the
  rest of the runtime's blocking operations behave), so a server can have many in-flight
  children without burning carrier threads.
- No guarantee of async-reactor integration in v1; that is future work (§1.4).

### 6.2 Use cases
- **6.2.1** As a server, I have N concurrent `jit_execute` fibers each waiting on a child,
  and they do not consume N carrier threads.

## 7. Errors & safety

### 7.1 Requirements
- **Launch failure** (program not found, permission denied, fork/CreateProcess failure)
  is reported as `launched == false` / `code() == -1` (§8.2), distinct from a normal
  non-zero exit (`launched == true`, `code() == N`). An OS-message-bearing diagnostic
  field on the result is a follow-up.
- **I/O errors** on a pipe (child died mid-stream) surface as the same I/O exceptions the
  `cajeta.io.file` reader/writer already raise.
- Resource cleanup is guaranteed: fds and child handles are released on `run()` return, on
  `Process` drop, and on exception.

### 7.2 Use cases
- **7.2.1** As a program, a launch failure gives me the program name and the OS reason.
- **7.2.2** As a program, an exception mid-run still releases all pipe fds and reaps the
  child.

### 7.3 Capability note (informative)
Spawning arbitrary processes is a privileged capability. cajeta already models capabilities
in the manifest and confines subprocesses in the build-tool sandbox; a future increment may
gate `cajeta.process` behind a declared capability and/or route it through the sandbox. v1
exposes the capability unguarded and documents the exposure.

## 8. Decision points (resolved at plan hand-off)
- **8.1** API granularity — **RESOLVED: ship both, dependency-ordered.** One-shot `run()`
  lands first as its own unit (it is all the cajeta MCP server's process-per-execute
  needs); streaming `spawn()` follows as a later unit. Both are in v1 scope.
- **8.2** Launch-failure surfacing — **RESOLVED (revised at implementation): result flag,
  no throw.** A failure to *start* the program (ENOENT, EACCES, spawn failure) returns a
  `ProcessResult` with `launched == false` and `code() == -1`; the caller inspects the
  flag. A normal non-zero exit likewise stays a result (`code() == N`). Rationale: (a)
  matches the host `Subprocess` model this spec mirrors; (b) serves the MCP server's
  inspect-don't-catch path; (c) the in-tree exception machinery currently constructs
  integer-sentinel throws even for `NetException` — a catchable `ProcessException`
  carrying program name + OS reason is a clean follow-up, not a v1 blocker. A throwing
  `runChecked()` variant may be added later.
- **8.3** Bridge implementation — **RESOLVED: `posix_spawn`.** Use `posix_spawn` on POSIX
  (simpler, avoids fork-in-threaded-process hazards with the fiber runtime), falling back
  as the host `Subprocess` does; `CreateProcess` on Windows.

## 9. Testing
- **Bridge + result mapping** — spawn real, tiny well-known programs (`true`/`false`, an
  echo, a sleeper) and assert `code()`, capture contents, signal/timeout flags. These are
  the end-to-end tests; they need the runtime but not the JIT.
- **Construction/validation** (empty argv, env-merge precedence, default stdio wiring) is
  pure logic — unit-tested directly.
- **Deadlock/large-output** (§3.2.4) and **timeout/kill** (§5.2.2) and **no-zombie on
  drop** (§5.2.3) each get a focused end-to-end test.
- Parity check against the host `Subprocess` semantics on shared cases (same argv → same
  `code()`).

## 10. Relationship to existing work
- Mirrors `src/cajeta/buildtool/Subprocess.h` (`runSubprocess`/`SubprocessOptions`/
  `SubprocessResult`) — the proven cross-platform model — but as a target-language stdlib
  package backed by new `__cajeta_proc_*` bridges (the host facility stays as the toolchain's
  own subprocess engine; the two are parallel layers, like the skill matcher and
  `cajeta.search`).
- Reuses `cajeta.io.file` (`File(fd)`, `FileReader`, `FileWriter`) for piped child stdio and
  `cajeta.time.Duration` for timeouts.
- Unblocks the **cajeta-written** `cajeta-mcp` server (`cajeta-mcp-spec.md`): once this
  lands, that server can orchestrate the toolchain subcommands and run process-per-execute
  entirely in cajeta.

## 11. Implemented v1 API (as shipped)
Reference: `docs/specification/process/Process.md`; tests: `test/process/ProcessTests.cpp`.
A few names diverge from the prose above for concrete reasons:
- **`Command.start()`** is the streaming launcher (this spec's `spawn()`) — `spawn` is a
  reserved fiber keyword in cajeta.
- **`Process.waitFor()`** (blocking) and **`Process.waitMillis(int64)`** (timeout) stand in
  for `wait()` / `wait(Duration)`; the `Duration` overload is deferred to avoid the
  `cajeta.time` coupling in v1.
- **Reaping is via `Process.close()`**, not a destructor (§5.1 "drop"): cajeta has no
  auto-close-on-drop destructor for stdlib classes yet (same as `FileReader`/`FileWriter`).
  `close()` kills a still-running child then reaps it, so no zombie leaks.
- **Launch failure is a result flag** (`launched()==false` / `code()==-1`), never a throw —
  see §8.2.
- **`waitFor()`/`run()` block the carrier thread** in v1, not the fiber (§6 fiber-yield
  deferred). Windows is a `launched()==false` stub pending the `CreateProcess` port.
