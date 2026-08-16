# Command

`cajeta.process.Command` — a subprocess command: a program + args, optional
working directory, environment, stdin bytes, capture/timeout settings.
Construction performs no OS work; `run()` spawns the child, performs the
configured stdio, waits for exit, and returns a `ProcessResult` (a `Command`
may be run more than once). For streaming interaction, wire pipes with
`pipeStdin`/`pipeStdout`/`pipeStderr` and call `start()` to get a live
[Process](Process.md) handle without waiting.

```cajeta
String[] argv = heap String[2];
argv[0] = "/bin/echo";
argv[1] = "hello";
Command cmd = heap Command(#argv);   // ownership of argv moves in
cmd.captureStdout();
ProcessResult r #= cmd.run();
```

## Methods

| Signature | |
|---|---|
| `Command(#String[] argv)` ⚑ | Build a command from a non-empty `argv` (`argv[0]` is the program, resolved against PATH); ownership of `argv` moves in |
| `void workingDir(#String dir)` | Run the child in `dir` instead of inheriting the parent's cwd |
| `void environment(#String[] entries)` | Replace the child's environment with `entries` (`"KEY=VALUE"` strings) |
| `void stdinBytes(#int8[] data, int32 len)` | Feed `data[0..len)` to the child's stdin (read to completion) |
| `void captureStdout()` | Capture the child's stdout into the result |
| `void captureStderr()` | Capture the child's stderr into the result |
| `void timeoutMillis(int64 ms)` | Kill the child and flag `timedOut` if it runs past `ms` |
| `void pipeStdin()` | Wire the child's stdin to a pipe (for streaming `start()`) |
| `void pipeStdout()` | Wire the child's stdout to a pipe (for streaming `start()`) |
| `void pipeStderr()` | Wire the child's stderr to a pipe (for streaming `start()`) |
| `#ProcessResult run()` ⚑ | Spawn the child, perform the configured stdio, wait for exit, return the result |
| `#Process start()` ⚑ | Spawn the child without waiting; returns a `Process` handle exposing piped stdio plus wait/kill/pid |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/process/Command.cajeta`](../../../runtime/src/cajeta/process/Command.cajeta)
- [Process](Process.md) — the live handle `start()` returns
