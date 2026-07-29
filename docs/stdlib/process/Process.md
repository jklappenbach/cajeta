# Process

`cajeta.process.Process` — a spawned, still-running child, returned by
[Command](Command.md)`.start()`. Wired pipe streams are exposed as
`File`-backed handles: write requests to `stdin()`, read responses from
`stdout()`/`stderr()` incrementally while the child runs, then `waitFor()` (or
`waitMillis()`) for the result. There is no auto-close-on-drop destructor yet:
call `close()` (or a blocking `waitFor()`) to reap the child — `close()` kills
a still-running child before reaping, so it never leaks a zombie.

```cajeta
String[] argv = heap String[1];
argv[0] = "/bin/cat";
Command cmd = heap Command(#argv);
cmd.pipeStdin();
cmd.pipeStdout();
Process p = cmd.start();
boolean ok = p.launched();
ProcessResult res = p.waitFor();
p.close();
```

## Methods

| Signature | |
|---|---|
| `Process()` | Default-construct an unspawned handle (the spawn bridge writes fields directly) |
| `boolean launched()` | Whether the child was successfully started |
| `#FileWriter stdin()` | A writer over the child's stdin pipe (when stdin was piped) |
| `#FileReader stdout()` | A reader over the child's stdout pipe (when stdout was piped) |
| `#FileReader stderr()` | A reader over the child's stderr pipe (when stderr was piped) |
| `#ProcessResult waitFor()` ⚑ | Block until the child exits and return its result |
| `#ProcessResult waitMillis(int64 ms)` | Wait up to `ms` milliseconds |
| `void kill()` | Terminate the child (SIGKILL) |
| `int32 pid()` | The child's process id, or -1 |
| `void close()` | Reap the child (killing it first if still running) and release the handle |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/process/Process.cajeta`](../../../runtime/src/cajeta/process/Process.cajeta)
- [Command](Command.md) — builds and starts the child;
  [FileReader](../io/file/FileReader.md) / [FileWriter](../io/file/FileWriter.md) — the piped stdio handles
