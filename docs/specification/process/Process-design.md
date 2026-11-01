# `cajeta.process` — Subprocess management (design)

`cajeta.process` is a sibling of `cajeta.io` and `cajeta.thread`, and it is
nested under neither, because a subprocess is its own concern, with a
fork/exec lifecycle, a signal model and an exit status. Stdin, stdout and
stderr stream through the same `InputStream` / `OutputStream` interfaces
used everywhere else, so the pumps are fiber-aware automatically.

This is a design document, and it is superseded in part. `cajeta.process`
shipped v1 with a different surface, `Command` / `ProcessResult` with blocking
waits, and [`Process.md`](Process.md) describes what exists. This document
keeps the fiber-aware direction that v1 deferred, which is a fiber-parking
`waitFor`, `Stdio` plumbing over [`cajeta.io.pipe`](../io/Pipes.md), and
capability gating.

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
    public #Process start();
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

Process p #= heap ProcessBuilder("ls")
    .arg("-la")
    .cwd(Path.of("/tmp"))
    .stdout(Stdio.PIPE)
    .start();

for (String line : p.stdout().reader().lines()) {
    println(line);
}

ExitStatus st #= p.waitFor();
if (!st.success()) {
    throw heap UnrecoverableException("ls failed with code " + st.code());
}
```

`@capability("process")` gates `start()`, because running subprocesses is a
distinct capability from filesystem or network access.

## Stdio plumbing

`Stdio.PIPE` is backed by the anonymous-pipe primitive in
[`cajeta.io.pipe`](../io/Pipes.md). The builder creates a `Pipe`, hands the child
the far end at fork/exec, and exposes the parent end as `stdin()`, an
`OutputStream`, and `stdout()` / `stderr()`, which are `InputStream`s.
`Stdio.FILE` redirects to a file fd, `Stdio.INHERIT` shares the parent's, and
`Stdio.NULL` uses `/dev/null`. See `Pipes.md` for the pipe ends, EOF and
backpressure, and the `SIGPIPE`→`IoException` behavior.

## Open items

The fiber-aware pieces land with the fiber reactor, where subprocess waits
park the calling fiber on SIGCHLD. v1 shipped without them, as
[`Process.md`](Process.md) § v1 limits records.
