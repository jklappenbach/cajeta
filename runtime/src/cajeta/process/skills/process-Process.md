---
id: process-Process
applies-to: [cajeta/process/Process]
title: Process — streaming handle for a spawned child
description: Live child handle from Command.start(); stdin/stdout/stderr pipe accessors plus waitFor/waitMillis/kill/pid, reaped manually via close().
---

# Process

The live handle to a still-running child returned by `Command.start()` (the
streaming counterpart of one-shot `Command.run()`). Use it to read/write the
child's piped stdio while it runs, then collect a `ProcessResult` via
`waitFor()` / `waitMillis()`. Part of `cajeta.process`.

**You do not construct this** — you receive an owned `#Process` from
`Command.start()`. (The public `Process()` ctor exists only so `start()`'s
parser-only stub body type-checks; the spawn bridge writes the fields directly.)
First check `launched()` — `false` means the spawn failed.

## Lifecycle — you MUST reap it

There is **no drop-on-scope destructor** (same limitation as `FileReader` /
`FileWriter`). A `Process` that leaves scope without being reaped leaks the OS
handle and zombies the child. Reap it exactly one of two ways:

- `void close()` — kills a still-running child first, then reaps and releases the
  handle, so it **never leaves a zombie**. Idempotent (safe to call after a
  `waitFor()`).
- `#ProcessResult waitFor()` — blocks until the child exits and returns the
  owned result; a blocking `waitFor()` reaps too. Still call `close()` afterward
  to release the handle (it's idempotent).

## Methods that matter

- `#FileWriter stdin()` — owned writer over the child's stdin pipe (only meaningful
  when `Command.pipeStdin()` was set). You own it; `close()` it to send EOF to the
  child.
- `#FileReader stdout()` / `#FileReader stderr()` — owned readers over the child's
  pipes (only when `pipeStdout()` / `pipeStderr()` was set). You own and must
  close them.
- `#ProcessResult waitFor()` — block until exit; owned result. See
  `cajeta/process/ProcessResult`.
- `#ProcessResult waitMillis(int64 ms)` — wait up to `ms`. If the child is still
  running at the deadline the result has `timedOut() == true` and the child is
  **left running** — call `kill()` then `waitFor()` to terminate and reap it.
- `void kill()` — SIGKILL the child (does not reap; follow with `waitFor()`/`close()`).
- `int32 pid()` — the child's pid, or `-1`.

`launched()` returns whether the spawn succeeded (handle non-zero).

## Worked example

```cajeta
import cajeta.process.Command;
import cajeta.process.Process;
import cajeta.process.ProcessResult;
import cajeta.io.file.FileReader;
import cajeta.io.file.FileWriter;
import cajeta.lang.String;

String[] argv = heap String[1];
argv[0] = "/bin/cat";
Command cmd = heap Command(#argv);
cmd.pipeStdin();
cmd.pipeStdout();
Process p #= cmd.start();              // owned handle; spawn happened
if (p.launched() == false) { return 100; }
FileWriter w #= p.stdin();
w.writeString("ping\n");
w.close();                            // EOF so cat finishes
FileReader rd #= p.stdout();
String s #= rd.readString(64);
ProcessResult res #= p.waitFor();      // owned result; reaps the child
p.close();                            // idempotent; releases the handle
```

Timeout-then-kill flow: `waitMillis(ms)` → if `timedOut()`, `kill()` →
`waitFor()` (result reports `signaled()`) → `close()`.

## Sharp edges

- After a `waitMillis()` timeout the child is **not** killed — you must `kill()`
  it yourself, or it keeps running.
- `stdin()/stdout()/stderr()` are only wired if the matching `pipe*()` flag was
  set on the `Command` before `start()`; otherwise the fd is `-1`.
- Field order (`handleVal`, `stdinFd`, `stdoutFd`, `stderrFd`) is the ABI the
  spawn/wait/kill/pid/close intrinsic lowering reads by offset
  (`MethodCallExpression.cpp`) — do not reorder.

## Does not

No auto-reap, no `spawn()` (the verb is `Command.start()` — `spawn` is a reserved
fiber keyword), no exceptions on launch failure (check `launched()`). Ownership-`#`
and launch-failure-as-value conventions are library-wide — see `cajeta.process`.
