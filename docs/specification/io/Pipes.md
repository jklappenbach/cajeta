# `cajeta.io.pipe` — Pipes (anonymous + named)

A pipe is a unidirectional, in-kernel **byte** stream with a read end and a
write end. Cajeta exposes two kinds:

- **Anonymous pipe** (`Pipe`) — an unnamed kernel pipe whose ends are passed by
  fd. The backbone of **subprocess stdio** (a child's stdin/stdout/stderr) and
  of fd-passing patterns. This is the high-value kind.
- **Named pipe / FIFO** (`Fifo`) — a filesystem rendezvous (`mkfifo`) that two
  *unrelated* processes open by path. POSIX-only, niche.

Status: **designed, not implemented.** Tracked in `Features.md`; implementation
plan in `plans/io/pipes-plan.md`. Sibling concern to `cajeta.io.file` and
`cajeta.process`.

## When to use a pipe (and when not to)

Pipes are a low-level **byte** transport. Reach for them only when a richer
primitive doesn't fit:

| Need | Use | Not |
|------|-----|-----|
| In-process, typed, structured streaming between fibers | [`Channel<T>`](../Concurrency.md) | a pipe |
| Talk to a child process's stdin/stdout/stderr | `Pipe` via `Stdio.PIPE` ([`Process.md`](../Process.md)) | a FIFO |
| Cross-process, bidirectional / structured IPC | a Unix-domain socket (preferred; reuses the `cajeta.io.net` socket + reactor machinery) | a FIFO |
| Cross-process, one-way byte rendezvous on a known path | `Fifo` (POSIX) | — |

`Channel<T>` already covers everything in-process — it is typed and structured,
strictly better than a byte pipe for fiber-to-fiber use. A pipe's unique value
is crossing the **process** boundary by **fd** (anonymous) or by **path**
(named). For anything bidirectional or structured across processes, a
Unix-domain socket beats a FIFO; the FIFO is for the genuinely simple
"one writer, one reader, one path" case.

## Anonymous pipe — `Pipe`

```cajeta
package cajeta.io.pipe;

import cajeta.io.InputStream;
import cajeta.io.OutputStream;

public final class Pipe {
    // Create an anonymous, unidirectional kernel pipe. The two ends are
    // INDEPENDENT, single-owner resources; move them out with `#`. Both ends
    // are non-blocking and registered with the cajeta.io.net reactor, so reads and
    // writes park the calling fiber instead of blocking its carrier.
    @capability("process")
    public static PipeEnds create();
}

// The two ends, returned together so ownership of each is explicit. Typical use
// is to immediately `#`-transfer one or both ends (e.g. into a ProcessBuilder).
public final class PipeEnds {
    public InputStream  reader;   // the read end
    public OutputStream writer;   // the write end
}
```

Each end is a single-owner handle over one fd. The reader is an `InputStream`
and the writer an `OutputStream` — the **same** interfaces `File` and `Process`
use — so the fiber-aware read/write pumps work automatically. Closing the write
end makes the reader observe EOF; closing the read end makes a subsequent write
fail (the `EPIPE`/`SIGPIPE` case is reported as an `IoException`, never a
process-killing signal — `SIGPIPE` is ignored process-wide).

Backpressure is the kernel pipe buffer (≈64 KiB on Linux): `write` parks when
the buffer is full, `read` parks when it is empty — the same shape as
`Channel`, but byte-level and process-crossing.

### Subprocess stdio (the primary consumer)

`Process` (see [`Process.md`](../Process.md)) builds on this primitive. When a
stream is configured `Stdio.PIPE`, the builder creates a `Pipe`, hands the child
the appropriate end at fork/exec, and exposes the parent end as
`process.stdin()` / `stdout()` / `stderr()`:

```cajeta
Process p = stack ProcessBuilder("wc")
    .arg("-l")
    .stdin(Stdio.PIPE)
    .stdout(Stdio.PIPE)
    .start();

p.stdin().writeString("a\nb\nc\n");
p.stdin().close();                 // child sees EOF
String out = p.stdout().readString();
p.waitFor();
```

User code rarely constructs a `Pipe` directly; it asks for `Stdio.PIPE` and the
plumbing is handled. `Pipe.create()` is the escape hatch for hand-rolled fd
wiring.

## Named pipe (FIFO) — `Fifo`

```cajeta
package cajeta.io.pipe;

import cajeta.io.file.Path;

public final class Fifo {
    // mkfifo(path) — create a FIFO special file. Mode defaults to 0666 & ~umask.
    // POSIX-only; throws on platforms without FIFOs.
    @capability("filesystem")
    public static void create(Path path);
    @capability("filesystem")
    public static void create(Path path, int32 mode);
}
```

A FIFO is just a path on disk after creation, so it is **opened with the
ordinary file API** — no new stream type:

```cajeta
Fifo.create(Path.of("/tmp/cajeta.fifo"));

// reader process:
FileReader r = File.openRead(Path.of("/tmp/cajeta.fifo"));   // blocks until a writer opens
// writer process:
FileWriter w = File.openWrite(Path.of("/tmp/cajeta.fifo"), OpenMode.WRITE);
```

Opening a FIFO for read blocks (or parks) until a writer opens it, and vice
versa — that rendezvous is the whole point. Beyond `create`, a FIFO needs no
dedicated surface; it rides `cajeta.io.file`.

## Ownership & lifetime

A pipe end owns one fd — a single-owner resource under the standard model
([`MemoryModel.md`](../MemoryModel.md)). Plain assignment borrows; `#name` moves
the end (e.g. into a `ProcessBuilder` or another scope). Each end is closed
independently with `close()` (idempotent). Auto-close-on-drop follows the same
trajectory as `FileReader`/`FileWriter` — required explicitly today, automatic
once the I/O destructor work lands.

## Platform notes

- **Anonymous pipes are portable.** Linux uses `pipe2(O_NONBLOCK | O_CLOEXEC)`;
  macOS/BSD `pipe()` + `fcntl`; Windows `CreatePipe` with overlapped I/O. Async
  readiness/completion rides the existing `cajeta.io.net` reactor (epoll / kqueue /
  IOCP), so no new event-loop machinery is needed.
- **FIFOs are POSIX-only.** Windows "named pipes" (`\\.\pipe\…`,
  `CreateNamedPipe`) are a *different* model (message mode, server/client
  semantics) — they are **not** POSIX FIFOs and are out of scope here. A future
  Windows-named-pipe or, better, a cross-platform **Unix-domain socket**
  (`cajeta.io.net`, including Windows AF_UNIX) is the preferred path for portable
  IPC and is tracked separately.

## See also

- [`Concurrency.md`](../Concurrency.md) — `Channel<T>`, the in-process choice.
- [`Process.md`](../Process.md) — subprocess management; the main `Pipe` consumer.
- [`io/file/File.md`](file/File.md) — the file API a FIFO is opened through.
- [`Io.md`](Io.md) — the I/O umbrella and the shared stream interfaces.
