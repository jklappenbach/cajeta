# cajeta.process

OS subprocess control for cajeta programs: build a command, run it to completion
with captured output (one-shot), or spawn a long-lived child with live piped
stdio plus wait / timeout / kill (streaming). Backed by `__cajeta_proc_*` C
bridges (`posix_spawn`-based on POSIX; a `CreateProcess` Windows port is stubbed),
intrinsic-lowered at the call site like `cajeta.io.file`.

Spec: `docs/specs/cajeta-process-spec.md`. Tests: `test/process/ProcessTests.cpp`
(the executable examples — every snippet below mirrors a passing test).

## Command

`Command` builds the invocation. Construction performs no OS work; `run()` /
`start()` is what touches the OS, and a `Command` may be run more than once.

| Method | Effect |
| --- | --- |
| `Command(#String[] argv)` | program + args; `argv[0]` is resolved against `PATH`. Ownership of `argv` moves in. |
| `workingDir(#String dir)` | run the child in `dir` instead of the parent's cwd. |
| `environment(#String[] entries)` | replace the child's env with `"KEY=VALUE"` entries (default: inherit). |
| `stdinBytes(#int8[] data, int32 len)` | feed `data[0..len)` to the child's stdin. |
| `captureStdout()` / `captureStderr()` | collect that stream into the `ProcessResult` (one-shot). |
| `timeoutMillis(int64 ms)` | kill the child and flag `timedOut` if it runs past `ms`. |
| `pipeStdin()` / `pipeStdout()` / `pipeStderr()` | wire that stream to a live pipe (streaming `start()`). |
| `#ProcessResult run()` | one-shot: spawn, do stdio, wait, return the result. |
| `#Process start()` | streaming: spawn without waiting, return a `Process`. (Named `start` because `spawn` is a reserved fiber keyword.) |

An empty `argv` is tolerated and surfaces as a launch failure
(`launched() == false`) at `run()`/`start()` time.

## ProcessResult

The outcome of a finished child.

| Method | Returns |
| --- | --- |
| `launched()` | whether the program started (false ⇒ not found / spawn failed). |
| `exited()` / `exitCode()` | normal exit + its code. |
| `signaled()` / `signal()` | signal-killed + the signal (POSIX). |
| `timedOut()` | the run/wait hit its deadline. |
| `code()` | exit code on normal exit, `128 + signal` if signaled, `-1` if never launched. |
| `stdout()` / `stderr()` | captured bytes (`int8[]`), when capture was requested. |

A **non-zero exit is a normal result**, not an error — inspect `code()`. A
**launch failure** is `launched() == false` / `code() == -1`, never a throw.

```cajeta
String[] argv = heap String[2];
argv[0] = "/bin/echo";
argv[1] = "hello";
Command cmd = heap Command(#argv);
cmd.captureStdout();
ProcessResult r = cmd.run();
if (r.launched() && r.code() == 0) {
    int8[] out = r.stdout();   // "hello\n"
}
```

## Process (streaming)

A spawned, still-running child. Pipe streams are exposed as `File`-backed
handles you read/write incrementally.

| Method | Effect |
| --- | --- |
| `launched()` | whether the child started. |
| `#FileWriter stdin()` | writer over the child's stdin pipe (when piped). |
| `#FileReader stdout()` / `stderr()` | readers over the child's output pipes (when piped). |
| `#ProcessResult waitFor()` | block until exit, return the result. |
| `#ProcessResult waitMillis(int64 ms)` | wait up to `ms`; on timeout the result has `timedOut()` and the child is left running. |
| `kill()` | terminate the child (SIGKILL). |
| `int32 pid()` | the child's process id, or -1. |
| `close()` | reap the child (killing it first if still running) and release the handle. |

There is **no auto-close-on-drop destructor** in cajeta yet (same as
`FileReader`/`FileWriter`): call `close()` (or a blocking `waitFor()`) to reap
the child. `close()` never leaks a zombie.

```cajeta
String[] argv = heap String[1];
argv[0] = "/bin/cat";
Command cmd = heap Command(#argv);
cmd.pipeStdin();
cmd.pipeStdout();
Process p = cmd.start();
FileWriter w = p.stdin();
w.writeString("ping\n");
w.close();                          // EOF to the child
FileReader rd = p.stdout();
String echoed = rd.readString(64);  // "ping\n"
ProcessResult res = p.waitFor();
p.close();
```

## v1 limits

- `run()`/`waitFor()` block the **carrier thread**, not the fiber. Fiber-yielding
  waits, async-reactor integration, pipelines/PTYs, and capability gating are
  future work (spec §1.4, §6, §7.3).
- Windows is a stub (`launched() == false`); the `CreateProcess` port lands with
  the Windows runtime.
