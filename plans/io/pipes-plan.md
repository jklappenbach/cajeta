# Plan: Pipes — anonymous (`Pipe`) + named (`Fifo`)

Status: **Planned.** Spec: `docs/stdlib/io/Pipes.md`. Sibling concern to
`cajeta.io.file` / `cajeta.process`.

## Context

A pipe is a unidirectional kernel **byte** stream with a read end and a write
end. Two kinds, very different value:

- **Anonymous `Pipe`** — the backbone of **subprocess stdio** (a child's
  stdin/stdout/stderr) and fd-passing. High value; the real reason to build
  pipes at all.
- **Named `Fifo`** — a filesystem (`mkfifo`) rendezvous between unrelated
  processes. POSIX-only, niche.

The design intentionally keeps pipes **narrow**: in-process streaming already
belongs to [`Channel<T>`](../../docs/stdlib/Concurrency.md), and cross-process
*structured* IPC should go to a Unix-domain socket (a separate `cajeta.net`
investment that reuses the socket + reactor machinery, and is portable to
Windows AF_UNIX) — not a FIFO. So this plan delivers the byte-level
process-crossing primitive and nothing more.

**Leverage that already exists** (de-risks the work):
- The cross-platform reactor (`cajeta.net.reactor`: epoll / kqueue / IOCP) —
  async readiness/completion for an fd is already solved; pipe fds register with
  the same engine.
- The fd-backed stream shape (`FileReader`/`FileWriter` over an fd) and the
  socket async read/write path — pipe ends reuse it.
- The C++ buildtool's `Subprocess.cpp` already does `pipe()` + fork/exec stdio
  redirection (compiler-internal); it is a working reference for the pattern.

**Dependencies / sequencing:** the `Pipe` primitive (Phase 1) and `Fifo`
(Phase 2) are self-contained and testable without a subprocess. The subprocess
integration (Phase 3) is gated on the (currently designed-not-built)
`cajeta.process` API ([`Process.md`](../../docs/stdlib/Process.md)) — pipes are
the enabling primitive it consumes.

## Scope

- **v1:** anonymous `Pipe` with async, fiber-parking ends; `Fifo.create`
  (POSIX) opened through the existing `File` API; subprocess `Stdio.PIPE` wiring
  once `Process` lands.
- **Out of scope:** Windows named pipes (`CreateNamedPipe` — a different model);
  Unix-domain sockets (separate, preferred for structured IPC); auto-close-on-
  drop (rides the shared `cajeta.io` destructor work, not pipe-specific).

---

## 1. TDD

a. **Anonymous `Pipe` — round-trip & semantics**

   1. [ ] In-process write→read preserves bytes (write end → read end, same fiber).
   2. [ ] Cross-fiber: a writer fiber and a reader fiber under `scope { spawn … }`;
      the reader parks until data arrives, then receives the full payload in order.
   3. [ ] EOF: closing the write end makes the reader observe end-of-stream
      (`read` returns 0 / `readString` completes), not a hang.
   4. [ ] Backpressure: a write larger than the kernel buffer parks until the
      reader drains; no deadlock under **either** `CAJETA_CARRIERS=1` or the
      default multi-carrier pool.
   5. [ ] Writing after the read end is closed surfaces an `IoException`
      (EPIPE), and the process does **not** die from `SIGPIPE`.

b. **Named `Fifo`**

   1. [ ] `Fifo.create(path)` produces a FIFO special file (`stat` reports
      `S_ISFIFO`); a reader and a writer opened via `File` rendezvous and
      round-trip a payload.
   2. [ ] `create` on an existing path, and on a platform without FIFOs, fails
      with a clean diagnostic — never a crash/UB.

c. **Subprocess integration** (gated on `cajeta.process`)

   1. [ ] `Stdio.PIPE` wires a real child's stdio through `Pipe` end-to-end
      (e.g. `wc -l`): the parent writes the child's stdin, closes it, and reads
      the child's stdout.

d. **Capability / platform**

   1. [ ] `@capability("process")` (pipe) and `@capability("filesystem")` (fifo)
      are honored where the sandbox applies.
   2. [ ] FIFO on a non-POSIX target reports "unsupported", not UB.

---

## 2. Deliverables

a. **Native runtime** · `runtime/native/`

   1. [ ] `__cajeta_pipe_create(int32 out_fds[2])` — `pipe2(O_NONBLOCK |
      O_CLOEXEC)` on Linux; `pipe()` + `fcntl` on macOS/BSD; `CreatePipe` +
      overlapped on Windows. Returns the read/write fds.
   2. [ ] `__cajeta_mkfifo(const char* path, int32 mode)` — POSIX `mkfifo`;
      a clear `ENOSYS`-style error on platforms without FIFOs.
   3. [ ] Reactor wiring for pipe fds — reuse the `cajeta.net.reactor`
      add/wait/remove helpers; ignore `SIGPIPE` process-wide at startup so a
      closed-reader write returns `EPIPE` instead of killing the process.

b. **Stream ends** · `cajeta.io` / `runtime/src/cajeta/io/`

   1. [ ] Read end as `InputStream`, write end as `OutputStream` over the pipe
      fd, fiber-parking through the reactor — reuse the `FileReader`/`FileWriter`
      fd-stream shape and the socket async read/write path. Close is independent
      per end and idempotent.

c. **Language surface** · `cajeta.io.pipe` / `runtime/src/cajeta/io/pipe/`

   1. [ ] `Pipe.create() -> PipeEnds { reader, writer }` — single-owner ends,
      `@capability("process")`; ends `#`-transferable.
   2. [ ] `Fifo.create(path)` / `Fifo.create(path, mode)` — `@capability("filesystem")`;
      opened thereafter via the ordinary `File` API (no new stream type).

d. **Subprocess wiring** · `cajeta.process` — *gated on the Process API*

   1. [ ] `Stdio.PIPE` in `ProcessBuilder` allocates a `Pipe`, hands the child
      the far end at fork/exec (CLOEXEC handling so only the intended end
      survives), and exposes the parent ends via `stdin()`/`stdout()`/`stderr()`.

e. **Docs** · `docs/`

   1. [x] `docs/stdlib/io/Pipes.md` — the spec (this change).
   2. [ ] Link it from `docs/stdlib/io/Io.md` (I/O umbrella) and
      `docs/stdlib/Process.md` (`Stdio.PIPE` → the pipe primitive).
   3. [ ] Add a Pipes row to `Features.md`.

---

## 3. Acceptance Criteria

a. [ ] Anonymous `Pipe` works in-process and cross-fiber with correct EOF and
   backpressure under both single- and multi-carrier scheduling.
b. [ ] `Fifo.create` + reader/writer rendezvous works on POSIX; a clean error
   on unsupported platforms.
c. [ ] No `SIGPIPE`-induced aborts; a write to a closed-reader pipe surfaces as
   an `IoException`.
d. [ ] (When `Process` lands) `Stdio.PIPE` round-trips through real child
   processes.
e. [ ] Capability annotations are enforced where the sandbox applies; in-process
   and structured-IPC users are pointed at `Channel<T>` / Unix-domain sockets,
   not pipes.

---

## Phasing

1. [ ] **Phase 1 — anonymous `Pipe`** (2.a.1, 2.a.3, 2.b, 2.c.1; TDD §1.a). The
   self-contained, high-value core.
2. [ ] **Phase 2 — `Fifo`** (2.a.2, 2.c.2; TDD §1.b). Small; POSIX-only.
3. [ ] **Phase 3 — subprocess `Stdio.PIPE`** (2.d; TDD §1.c). Lands with /
   after the `cajeta.process` plan; pipes are its enabling primitive.

## Verification

a. [ ] Build the compiler + tests; embed-runtime rebuild picks up the new
   `cajeta.io.pipe` sources.
b. [ ] Run the pipe tests in isolation, then under `CAJETA_CARRIERS=1` and the
   default pool, to prove the parking/backpressure paths on both.
