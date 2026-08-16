---
id: process-Command-run
applies-to: [cajeta/process/Command.run]
title: Command.run() — one-shot spawn+wait, returns an owned ProcessResult
description: Run a subprocess to completion; set captures before run(); never throws on launch failure — check launched().
---

# `Command.run() -> #ProcessResult`

Spawns the configured child, performs the configured stdio, **waits for it to
exit**, and returns an **owned** `#ProcessResult` (heap, caller frees). One call =
spawn + wait. For non-blocking/streaming use `start()` instead (returns a `Process`).

## The protocol — configure, THEN run

`run()` reads only the state already set on the `Command`. Every configuration call
(`captureStdout()`, `captureStderr()`, `stdinBytes()`, `workingDir()`,
`environment()`, `timeoutMillis()`) must happen **before** `run()`; calling them
after does nothing for that run. Most common trap: calling `run()` first and then
reading `r.stdout()` — without a prior `captureStdout()`, `stdout()` returns `null`.

A `Command` is reusable: you may call `run()` more than once on the same instance.

## It never throws on launch failure — check `launched()`

A program that can't start (not found, empty `argv`, etc.) does **not** throw. It
returns normally with `r.launched() == false` and `r.code() == -1`. A **non-zero
exit is also not an error** — it is a normal result; read `r.code()` / `r.exitCode()`.
So the first thing after `run()` is always an `r.launched()` check.

## Return ownership & null semantics

- The returned `#ProcessResult` is heap-owned by the caller.
- `r.stdout()` / `r.stderr()` (`int8[]`) are populated **only** when the matching
  `captureStdout()` / `captureStderr()` was set before `run()`; otherwise `null`.
  Use `out.count()` for the captured length.
- See `cajeta/process/ProcessResult` for the full result surface
  (`exited()`, `signaled()`, `signal()`, `timedOut()`, `code()`).

## Side effects

Spawns an OS process and blocks the calling fiber until it exits (or until the
`timeoutMillis(ms)` deadline, after which the child is killed and `r.timedOut()`
is true). `argv[0]` is resolved against `PATH`.

## What it does NOT do

- Does not stream — it waits for full completion. Need incremental stdio? use
  `Command.start()` → `cajeta/process/Process`.
- Does not throw — no exception on missing program or non-zero exit.
- Does not capture stdio by default — uncaptured streams are inherited from the parent.

## Example (mirrors `test/process/ProcessTests.cpp`)

```cajeta
import cajeta.process.Command;
import cajeta.process.ProcessResult;
import cajeta.lang.String;

String[] argv = heap String[2];
argv[0] = "/bin/echo";
argv[1] = "hello";
Command cmd = heap Command(#argv);   // ownership of argv moves into cmd
cmd.captureStdout();                  // MUST precede run()
ProcessResult r #= cmd.run();          // spawn + wait; r is owned by us
if (r.launched() == false) { return 100; }
if (r.code() != 0) { return 1; }
int8[] out = r.stdout();              // non-null only because we captured
// out.count() == 6, out[5] == 10 ('\n')
```
