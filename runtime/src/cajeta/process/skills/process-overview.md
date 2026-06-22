---
id: process-overview
applies-to: [cajeta.process]
title: cajeta.process — running subprocesses (one-shot run vs streaming start)
description: Orientation and task routing for spawning POSIX subprocesses with Command/Process/ProcessResult.
---

# cajeta.process — subprocess execution

Build a `Command` (program + args + options), then either **run it to completion**
or **spawn it and stream**. POSIX-only. Construction does no OS work; the OS is
touched only by `run()`/`start()`.

## Task → entry point

| I want to…                                              | Start with |
| ------------------------------------------------------- | ---------- |
| Configure a program + args + cwd/env/stdin/capture      | `Command` (construct with `#argv`) |
| Run to completion and collect exit code + captured output | `Command.run()` → `ProcessResult` |
| Spawn and stream stdio while the child runs             | `Command.start()` → `Process` |
| Read exit status / signal / timeout / captured bytes    | `ProcessResult` |
| Read/write a live child's pipes, wait, kill, get pid    | `Process` |
| Detect a failed launch (program not found, empty argv)  | check `launched() == false` (NOT a thrown exception) |
| Auto-close a `Process` when it leaves scope             | **not provided** — call `Process.close()` (or a blocking `waitFor()`) yourself |
| A `spawn()` method                                      | **not provided** — it is `start()` (`spawn` is a reserved fiber keyword) |
| Pipelines / shell parsing / glob expansion              | **not provided** — pass exact argv; run `/bin/sh -c "…"` if you need a shell |

## Run one-shot vs stream

- **`run()`** — blocks, does all configured stdio, waits for exit, returns a
  `ProcessResult`. Use when you want the whole output at the end. Capture with
  `captureStdout()` / `captureStderr()`.
- **`start()`** — returns immediately with a `Process` handle exposing the piped
  stdio. Use when you must read/write incrementally while the child runs. Wire
  pipes first with `pipeStdin()` / `pipeStdout()` / `pipeStderr()`.

## Library-wide invariants

- **Ownership transfer (`#`).** A leading `#` on a parameter moves ownership into
  the command: `Command(#argv)`, `workingDir(#dir)`, `environment(#entries)`,
  `stdinBytes(#data, len)`. Pass the `#` at the call site (`cmd.run` lowering and
  the test corpus both rely on it). After the move the caller no longer owns the
  array/string.
- **Returns are owned (`#`).** `run()`, `start()`, `waitFor()`, `waitMillis()`,
  and `Process.stdin/stdout/stderr` all return heap-owned objects (`#ProcessResult`,
  `#Process`, `#FileWriter`, `#FileReader`). `ProcessResult.stdout()/stderr()`
  return the result's own `int8[]` (borrowed view — copy to outlive the result);
  they are `null` unless capture was requested.
- **No drop-on-scope.** There is no destructor that reaps a child or closes a fd.
  A `Process` must be reaped via `close()` (kills a still-running child first, so
  no zombie) or a blocking `waitFor()`. `close()` is idempotent. Same limitation
  applies to the `FileReader`/`FileWriter` you get from its pipes.
- **Launch failure is a value, not a throw.** Program-not-found, empty argv, etc.
  return `launched() == false` and `code() == -1`. Always check `launched()`
  before trusting the rest of the result. A non-zero exit is a *normal* result,
  not a failure — inspect `code()`.
- **Pinned ABI field order.** `Command`, `Process`, and `ProcessResult` have a
  fixed field order that the intrinsic lowering in `MethodCallExpression.cpp`
  reads by byte offset. Do not reorder fields.
- **`Command` is reusable.** It holds only config; it may be `run()` more than once.

## Canonical example

```cajeta
import cajeta.process.Command;
import cajeta.process.ProcessResult;
import cajeta.lang.String;

String[] argv = heap String[2];
argv[0] = "/bin/echo";
argv[1] = "hello";
Command cmd = heap Command(#argv);   // argv ownership moves into cmd
cmd.captureStdout();                  // request stdout capture
ProcessResult r = cmd.run();          // blocks until exit
if (r.launched() == false) { return 100; }  // launch failure, not a throw
int8[] out = r.stdout();             // borrowed bytes (null if not captured)
int32 status = r.code();             // exit code, or 128+signal, or -1
```

## Status codes

`ProcessResult.code()` normalizes status: the exit code on a normal exit,
`128 + signal` when signal-killed, `-1` when the program never launched.
`launched()`, `exited()`, `signaled()`, `timedOut()` flag the specific case.

## Downward pointers

- `Command` — full option list (working dir, env, stdin bytes, timeout, pipe vs
  capture flags) and the two launch verbs.
- `Process` — streaming handle: `stdin()/stdout()/stderr()`, `waitFor()`,
  `waitMillis()`, `kill()`, `pid()`, `close()`.
- `ProcessResult` — finished-run outcome accessors.
