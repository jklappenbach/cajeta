---
id: process-Command
applies-to: [cajeta/process/Command]
title: Command — build and run a subprocess
description: Builder for a subprocess; configure cwd/env/stdin/capture/pipe/timeout, then run() one-shot or start() streaming.
---

# Command

The **main access point** for `cajeta.process`: a reusable builder for one
subprocess. You construct it with the `argv`, call configuration setters to
tune it, then either `run()` it to completion (one-shot, returns a
`#ProcessResult`) or `start()` it for streaming (returns a `#Process` handle).
Construction does **no** OS work — nothing is spawned until `run()`/`start()`.

## Construct

```cajeta
import cajeta.process.Command;
import cajeta.process.ProcessResult;
import cajeta.lang.String;

String[] argv = heap String[2];
argv[0] = "/bin/echo";   // argv[0] is the program, resolved against PATH
argv[1] = "hello";
Command cmd = heap Command(#argv);   // ownership of argv moves into the Command
cmd.captureStdout();
ProcessResult r #= cmd.run();
if (r.launched() && r.code() == 0) {
    int8[] out = r.stdout();
}
```

`Command(#String[] argv)` — argv is **taken** (`#` move): do not reuse the
array after constructing. An empty argv is tolerated; it surfaces as a launch
failure (`ProcessResult.launched() == false`, `code() == -1`) at run time, not
a throw.

## Configure (all setters mutate the receiver and `#`-take their args)

- `workingDir(#String dir)` — run the child in `dir` instead of inheriting cwd.
- `environment(#String[] entries)` — replace the env with `"KEY=VALUE"` strings;
  without it the child inherits the parent's env.
- `stdinBytes(#int8[] data, int32 len)` — feed `data[0..len)` to the child's
  stdin (read to completion).
- `captureStdout()` / `captureStderr()` — capture that stream into the
  `ProcessResult` (read it back via `ProcessResult.stdout()`/`stderr()`).
- `timeoutMillis(int64 ms)` — kill the child and set `timedOut()` if it overruns.
- `pipeStdin()` / `pipeStdout()` / `pipeStderr()` — wire a pipe for streaming;
  only meaningful with `start()`.

Capture vs pipe are distinct: **capture** buffers the whole stream for `run()`;
**pipe** exposes a live `File`-backed stream on the `Process` from `start()`.

## Run

- `#ProcessResult run()` — spawn, do the configured stdio, wait, return the
  result. **You own** the returned `ProcessResult`. A non-zero exit is a normal
  result (check `code()`/`exited()`), not an error. See `cajeta/process/ProcessResult`.
- `#Process start()` — spawn without waiting; returns a live `#Process` handle
  for piped stdio + `waitFor()`/`kill()`/`close()`. **You own** it and **must**
  `close()` (or a blocking `waitFor()`) it — there is no drop-on-scope reaper, so
  skipping it leaks/zombies the child. See `cajeta/process/Process`.

Note: the streaming method is `start()`, **not** `spawn()` (`spawn` is a reserved
fiber keyword).

## State & reuse

A `Command` is mutable and **reusable**: configure once, `run()` it more than
once. The setters are flag/field assignments with no OS effect.

## Does not

No `mkdir`/cwd creation (a missing `workingDir` surfaces as a launch failure),
no shell parsing (argv is exec'd directly — wrap in `/bin/sh -c "…"` if you need
a shell), no incremental output from `run()` (use `start()` + pipes). Do not
reorder the public fields: their order is the ABI read by the `run()`/`start()`
intrinsic lowering.
