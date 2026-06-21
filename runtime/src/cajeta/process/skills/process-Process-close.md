---
id: process-Process-close
applies-to: [cajeta/process/Process.close]
title: Process.close — reap the child and release the handle
description: Kills a still-running child then reaps it and frees the handle; idempotent; the mandatory cleanup since there is no drop-on-scope reaper.
---

# Process.close()

`public void close()` — **reap protocol** for a `#Process` from `Command.start()`.
If the child is still running it is killed (SIGKILL) **first**, then reaped
(blocking `waitpid`), then the opaque handle is released (`handleVal` → 0). This
is the cleanup you **must** call on every `Process` you do not fully wait out —
cajeta has **no** drop-on-scope destructor, so a skipped `close()` leaks the
handle and zombies the child.

Intrinsic-lowered to `__cajeta_proc_release`; the cajeta body is a parser-only
stub.

## Semantics & call sequence

- Takes no parameters, returns nothing.
- **Idempotent**: calling it on an already-closed (or never-launched) handle is a
  no-op — safe to call after a blocking `waitFor()` already reaped the child.
- Order in the lifecycle: `Command.start()` → (optional `stdin()/stdout()/stderr()`
  I/O, `pid()`, `kill()`, `waitFor()`/`waitMillis()`) → `close()`. Always last.
- After `close()` the handle is dead: `pid()` → -1, further pipe/`waitFor()` calls
  are meaningless. Do not reuse the `Process`.

## When you still need it

- After `waitFor()` (which already reaps): `close()` is still correct and cheap —
  it just releases the handle. Idiomatic to call it anyway.
- After `waitMillis()` that **timed out** (`timedOut() == true`): the child is left
  running — `close()` is what kills+reaps it.
- After `kill()`: the child is signaled but not yet reaped; `close()` reaps it.

## Side effects

Sends SIGKILL to a live child, blocks to `waitpid` it (no zombie), closes any
still-open pipe fds owned by the handle, zeroes `handleVal`. Does **not** flush or
close a `#FileWriter`/`#FileReader` you obtained from `stdin()/stdout()/stderr()` —
those are separately owned; close the writer yourself before reaping (see the
example).

## Does not

No exceptions or error codes — `close()` cannot fail from cajeta's view. It does
**not** return the exit status; capture that from `waitFor()`/`waitMillis()` →
`ProcessResult` *before* closing if you need it.

## Example (mirrors test/process/ProcessTests.cpp closeReapsChild / cat round-trip)

```cajeta
import cajeta.process.Command;
import cajeta.process.Process;
import cajeta.process.ProcessResult;
import cajeta.io.file.FileWriter;
import cajeta.io.file.FileReader;
import cajeta.lang.String;

String[] argv = heap String[1];
argv[0] = "/bin/cat";
Command cmd = heap Command(#argv);   // argv moves into cmd
cmd.pipeStdin();
cmd.pipeStdout();

Process p = cmd.start();             // you own p; you MUST close it
if (p.launched() == false) { return 100; }

FileWriter w = p.stdin();
w.writeString("ping\n");
w.close();                           // close the writer yourself; p.close() won't

FileReader r = p.stdout();
String line = r.readString(64);
ProcessResult res = p.waitFor();     // reaps + gives exit status
p.close();                           // idempotent here; releases the handle
```

To abandon a long-running child, skip `waitFor()` — `p.close()` alone kills and
reaps it (the `/bin/sleep 30` case: the pid no longer exists afterward).

See `cajeta/process/Process` (handle), `cajeta/process/Command.start` (origin),
and the library invariants in `cajeta.process`.
