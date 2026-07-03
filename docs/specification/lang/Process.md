# `cajeta.process` — Subprocess management

Sibling of `cajeta.io` and `cajeta.thread`, not nested under either —
a subprocess is its own concern (fork/exec lifecycle, signal model,
exit status). Stdin / stdout / stderr stream through the same
`InputStream` / `OutputStream` interfaces used everywhere else, so
the pumps are fiber-aware automatically.

Status: **designed, not implemented**. Tracked in Features.md.

## Surface

```cajeta
public enum Stdio { INHERIT, PIPE, NULL, FILE }

public final class ProcessBuilder {
    public ProcessBuilder(String command);
    public ProcessBuilder arg(String s);
    public ProcessBuilder args(String... s);
    public ProcessBuilder env(String key, String value);
    public ProcessBuilder cwd(Path dir);
    public ProcessBuilder stdin(Stdio mode);
    public ProcessBuilder stdout(Stdio mode);
    public ProcessBuilder stderr(Stdio mode);
    @capability("process")
    public Process start();
}

public final class Process {
    public int32  pid();
    public OutputStream stdin();            // if Stdio.PIPE
    public InputStream  stdout();           // if Stdio.PIPE
    public InputStream  stderr();           // if Stdio.PIPE
    public ExitStatus   waitFor();          // fiber-parks
    public ExitStatus   waitFor(Duration timeout);
    public void         terminate();        // SIGTERM
    public void         kill();             // SIGKILL
    public void         signal(Signal sig);
}

public final class ExitStatus {        // a small value-carrier (plain class, like Duration)
    public int32 code();
    public boolean success();
    public Signal terminatedBy();           // null if exited normally
}
```

## Example

```cajeta
import cajeta.process.ProcessBuilder;
import cajeta.process.Stdio;

Process p = heap ProcessBuilder("ls")
    .arg("-la")
    .cwd(Path.of("/tmp"))
    .stdout(Stdio.PIPE)
    .start();

for (String line : p.stdout().reader().lines()) {
    println(line);
}

ExitStatus st = p.waitFor();
if (!st.success()) {
    throw heap UnrecoverableException("ls failed with code " + st.code());
}
```

`@capability("process")` gates `start()` — running subprocesses is a
distinct capability from filesystem or network access.

## Stdio plumbing

`Stdio.PIPE` is backed by the anonymous-pipe primitive in
[`cajeta.io.pipe`](../io/Pipes.md): the builder creates a `Pipe`, hands the child
the far end at fork/exec, and exposes the parent end as `stdin()` (an
`OutputStream`) / `stdout()` / `stderr()` (`InputStream`s). `Stdio.FILE`
redirects to a file fd; `Stdio.INHERIT` shares the parent's; `Stdio.NULL` uses
`/dev/null`. See `Pipes.md` for the pipe ends, EOF/backpressure, and the
`SIGPIPE`→`IoException` behavior.

## Open items

All of `cajeta.process` is unimplemented. Tracked in Features.md.
Lands with the fiber reactor (subprocess waits park the calling
fiber on SIGCHLD).
