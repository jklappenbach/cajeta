---
id: process-ProcessResult
applies-to: [cajeta/process/ProcessResult]
title: ProcessResult — inspect a finished subprocess
description: Value type returned by Command.run()/Process.waitFor(); read code(), launched(), stdout()/stderr().
---

`ProcessResult` is the **outcome of a finished child process** in `cajeta.process`.
It is a **support/value type** — you never construct it yourself; you receive it
(`#`-owned) from `Command.run()`, `Process.waitFor()`, or `Process.waitMillis()`.
A non-zero exit is a normal result, not an error; a failure to *launch* (program
not found, empty argv) is **not** thrown — it comes back as `launched() == false`.

## Decide what to read

- **Just want one number?** Use `code()` — normalized status: the exit code on a
  normal exit, `128 + signal` when signal-killed, `-1` when it never launched.
- **Need to distinguish launch failure from a real exit?** Check `launched()`
  first — `false` means the OS never ran the program.
- **Normal exit vs. killed?** `exited()` true → use `exitCode()`; `signaled()`
  true → use `signal()`. From `waitMillis()`, `timedOut()` true means the child
  is still running (it was not killed; call `kill()` yourself).

## Methods that matter

- `boolean launched()` — program was successfully started.
- `int32 code()` — normalized status (exit / `128+signal` / `-1`). Most callers want this.
- `boolean exited()` / `int32 exitCode()` — normal exit and its raw code.
- `boolean signaled()` / `int32 signal()` — killed by a signal (POSIX) and its number.
- `boolean timedOut()` — the run hit its timeout (only meaningful from `waitMillis`/`timeoutMillis`).
- `int8[] stdout()` / `int8[] stderr()` — captured bytes, or **`null`**.

## Ownership & lifecycle

- The result is `#`-returned: you **own** it; it is a plain heap object with no
  `close()` and no drop-on-scope. Let it fall out of scope.
- `stdout()`/`stderr()` return the result's own `int8[]` (a borrowed view, not a
  copy) — valid for the result's lifetime. They are **`null` unless capture was
  requested** on the `Command` via `captureStdout()`/`captureStderr()`. Always
  null-check before indexing; use `out.count()` for length.

## What it does NOT do

- It does **not** throw on a non-zero exit or a launch failure — there is no
  exception path; branch on `launched()`/`code()`.
- It does **not** decode bytes to text — `stdout()`/`stderr()` are raw `int8[]`.
- It does **not** capture output by default — without `captureStdout()` on the
  `Command`, `stdout()` is `null` (the child's stdio was inherited).

## Example (with imports)

```cajeta
package test;
import cajeta.process.Command;
import cajeta.process.ProcessResult;
import cajeta.lang.String;

String[] argv = heap String[2];
argv[0] = "/bin/echo";
argv[1] = "hello";
Command cmd = heap Command(#argv);
cmd.captureStdout();                 // required, or stdout() is null
ProcessResult r = cmd.run();
if (r.launched() == false) {         // launch failure, code() == -1
    return 100;
}
int8[] out = r.stdout();             // borrowed view; "hello\n" -> count()==6
return r.code();                     // 0 on success
```

See `cajeta/process/Command` (construct + run), `cajeta/process/Process`
(`waitFor()`/`waitMillis()` also return this type).
