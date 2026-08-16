---
id: process-streaming
applies-to: [cajeta/process/Command, cajeta/process/Process]
title: Process streaming — pipe stdio while the child runs
description: Choreograph Command + Process + FileReader/FileWriter to write stdin and read stdout/stderr of a live subprocess, then waitFor and close.
---

# Streaming a live subprocess

Use this when you must **interleave writing input and reading output while the child
runs** (e.g. feed a filter and read its transformed output) — not when you have all
input up front and want all output at the end. For that one-shot case use
`Command.run()` with `stdinBytes`/`captureStdout` instead (see `cajeta/process/Command`
and `cajeta/process/ProcessResult`); it is simpler and cannot deadlock.

## Members and roles

- `Command` — configure the pipes (`pipeStdin()`/`pipeStdout()`/`pipeStderr()`) and
  launch with `start()`. (Builder details: `cajeta/process/Command`.)
- `Process` — the live handle returned by `start()`. Exposes the piped fds as
  `File`-backed streams plus `waitFor()`/`waitMillis()`/`kill()`/`pid()`/`close()`.
- `FileWriter` — wraps the child's **stdin** pipe (from `Process.stdin()`).
- `FileReader` — wraps the child's **stdout**/**stderr** pipe (from
  `Process.stdout()`/`stderr()`). Both are the same types used for files; see
  `cajeta/io/file/FileWriter` and `cajeta/io/file/FileReader` for the read/write loop.
- `ProcessResult` — final outcome from `waitFor()`/`waitMillis()` (see
  `cajeta/process/ProcessResult`).

## Object graph & ownership across the boundary

`Command.start()` returns a heap-owned `#Process`. From it:

- `stdin()`/`stdout()`/`stderr()` each construct a **fresh, heap-owned** `#FileWriter`
  / `#FileReader` over the corresponding pipe fd. **You own each one and must
  `close()` it** — there is no drop-on-scope reaper. Call each accessor **once** and
  keep the handle; every call re-wraps the same fd.
- Closing the `FileWriter` from `stdin()` **closes the child's stdin pipe, signalling
  EOF** to the child. Do this once you have written all input — a child that reads to
  EOF (e.g. `/bin/cat`) will not finish or flush its output otherwise.
- `readString(max)` returns an **owned `#String`** backed by a fresh buffer, so it
  stays valid after you close the reader. `read(buf, max)` fills your buffer and
  returns the count; `0` means EOF.
- `waitFor()`/`waitMillis(ms)` return an **owned `#ProcessResult`**. `launched()` is a
  value, never a throw — a failed spawn comes back as `launched() == false`.

## Call sequence

1. `pipeStdin()` / `pipeStdout()` / `pipeStderr()` on the `Command` (only the streams
   you need). These are distinct from `captureStdout/Stderr`, which are for `run()`.
2. `Process p #= cmd.start();` then check `p.launched()`.
3. `FileWriter w #= p.stdin();` write with `writeString`/`write`, then `w.close()` to
   send EOF.
4. `FileReader r #= p.stdout();` read with `readString`/`read` (loop until `read`
   returns 0), then `r.close()`.
5. `ProcessResult res #= p.waitFor();` (or `waitMillis` to poll — a deadline hit leaves
   the child running with `res.timedOut() == true`; `kill()` then `waitFor()` to reap).
6. `p.close();` — always. It kills a still-running child first then reaps (no zombie)
   and releases the handle; it is idempotent, so calling it after `waitFor()` is fine.

**Deadlock hazard:** with both stdin and stdout piped, writing more than a pipe
buffer (~64 KiB) of input *before* draining stdout can block both sides forever. For
bounded I/O, write then `close()` stdin before reading (as below); for large
bidirectional streams, interleave or drain on a separate fiber.

## Worked example (mirrors `test/process/ProcessTests.cpp::spawnWriteReadWait`)

```cajeta
import cajeta.process.Command;
import cajeta.process.Process;
import cajeta.process.ProcessResult;
import cajeta.io.file.FileWriter;
import cajeta.io.file.FileReader;
import cajeta.lang.String;

String[] argv = heap String[1];
argv[0] = "/bin/cat";
Command cmd = heap Command(#argv);   // argv ownership moves into cmd
cmd.pipeStdin();                      // wire stdin + stdout pipes (NOT capture*)
cmd.pipeStdout();

Process p #= cmd.start();              // spawn, do not wait; #Process owned by you
if (p.launched() == false) { return 100; }

FileWriter w #= p.stdin();            // owned writer over the child's stdin pipe
w.writeString("ping\n");
w.close();                            // closes stdin -> sends EOF so cat finishes

FileReader rd #= p.stdout();          // owned reader over the child's stdout pipe
String s #= rd.readString(64);        // owned String; survives rd.close()
rd.close();

ProcessResult res #= p.waitFor();     // owned result; blocks for exit
p.close();                            // always reap + release the handle (idempotent)

if (res.code() != 0) { return 1; }
return 0;
```

## Does not

No auto-close on scope for `Process`, `FileWriter`, or `FileReader` — close each
yourself. No shell, glob, or pipeline wiring (`cmd | cmd`): `start()` runs one argv;
chain processes by hand. `waitFor()` does not release the handle — still call
`close()`. `flush()` on the stdin writer is a no-op (writes are unbuffered); only
`close()` signals EOF.
