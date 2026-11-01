# The Jupyter kernel — `cajeta kernel`

Cajeta runs as a Jupyter kernel. One kernel process hosts one persistent
JIT session, and each cell compiles into that session and can see
everything the cells before it declared. Bindings, types, and top-level
methods persist across cells. Output streams while a cell runs, and a
cell that throws does not take the kernel with it.

It works with Jupyter Lab, classic Notebook, `jupyter console`, VSCode's
notebook UI, and anything else speaking Jupyter messaging v5.3.

## Install

```
cajeta init --kernel
```

This writes a kernel spec into your Jupyter data directory
(`~/.local/share/jupyter/kernels/cajeta/kernel.json` on Linux,
`~/Library/Jupyter/...` on macOS, `%APPDATA%\jupyter\...` on Windows),
pointing at the `cajeta` binary you ran it from. Confirm it took:

```
jupyter kernelspec list
```

It will not overwrite an existing spec. Pass `--force` if you mean to
replace one, for example after moving the binary.

Then run `jupyter lab` and pick Cajeta from the launcher.

## Running it by hand

```
cajeta kernel                       # generate a connection file and print it
cajeta kernel -f connection.json    # use the frontend's connection file
```

With no connection file, the kernel binds free ports, writes a
connection file into your Jupyter runtime directory, and prints the path
along with the command to attach:

```
jupyter console --existing kernel-cajeta-52913.json
```

That file is removed on exit. A connection file you supply belongs to
you and is left alone.

## What a cell is

A cell is a script unit, which is top-level statements, methods, and type
declarations freely mixed, with no enclosing class. The language rules
are in `specs/script-units-spec.md`, and the short version follows:

```cajeta
// In[1]
public class Point {
    public int32 x;
    public int32 y;
    public Point(int32 x, int32 y) { this.x = x; this.y = y; }
}
Point origin = heap Point(3, 4);
```

```cajeta
// In[2] — sees Point and origin
origin.x + origin.y;        // Out[2]: 7
```

A trailing expression is the cell's result, and it renders as `Out[N]`.
Primitives and `String` render directly, and an object renders through
its `toString()`, including a `@ToString`-synthesized one. A cell ending
in a declaration, a loop, a `return`, or a void call has no result and
displays nothing. Neither does an assignment, since `x = 1` is a
statement and has no value.

When a rendered value is itself JSON, meaning an object or an array, the
result also carries an `application/json` bundle for frontends that
render structured output. Scalars deliberately do not, because an
`application/json` of `42` adds noise and no capability.

**Redefinition is generational.** Re-declaring a type in a later cell
creates a new generation and leaves the old one unchanged. Values
created from the earlier generation keep their own layout and methods,
and they do not silently reinterpret themselves under the new
definition.

## Errors

A cell that fails to compile, and a cell that throws, both come back as
a Jupyter `error` with a type, a message, and a traceback. Traceback
frames name cells, and never the synthesized classes cells compile into:

```
Exception: boom
  In[4], line 1
```

The session survives either. A failed compile leaves it completely
untouched, and an uncaught throw unwinds the cell and leaves the
session's bindings intact.

The exception is an unrecoverable error, which is a panic and never a
cell error. The kernel reports it and exits, because continuing over a
world the runtime has already declared broken is worse than stopping.

Warnings reach the notebook as `stderr` output. Only the cell's own
diagnostics are shown. The session's first compile pulls the stdlib
through the same machinery, and none of that is yours to read.

## Interrupt

Interrupting from the frontend (the stop button, or `i i` in a
console) stops the running cell at its next statement boundary. The
cell ends as a `KeyboardInterrupt` error, the session and its bindings
survive, and the next cell runs normally.

The kernel stays responsive while a cell is stuck. Interrupts are
handled off the execution thread, so the control channel answers even
in the middle of a runaway loop.

**Limitation.** Interruption is safepoint-granular, and safepoints exist
in your cell's code. A cell parked inside a long stdlib call or a native
call will not stop until it returns to its own code. An interrupt sent
while nothing is running is a no-op.

## Restart

Restarting drops every session binding, running destructors, resets the
execution counter to 1, and starts from an empty session. Names bound
before the restart are gone.

## Projects and dependencies

Launch the kernel from inside a project directory and that project's
`cajeta.json` classpath applies, so a notebook can import project
dependencies and Olla libraries exactly as a compiled program would.

## Security

Messages are HMAC-SHA256 signed with the key from the connection file,
per the Jupyter protocol. A message whose signature does not verify is
dropped. It is never answered, never logged back to the sender, and
never dispatched. An empty key is the protocol's explicit unsigned mode.

## Limitations in v1

- There is no completion or introspection. `Tab` and `?` return empty
  results so the frontend never hangs, but there is no engine behind
  them yet.
- There is no `stdin`, so a cell cannot prompt for input.
- Rendering is text plus the JSON bundle described above. Images and
  HTML tables are staged behind display protocols in a later revision.
- There is no debugger bridge yet. Breakpoints, pause, and step inside a
  notebook are the v1.5 layer designed in
  `docs/specification/debugging/Debugging.md`.
