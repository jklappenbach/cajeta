# The Jupyter kernel — `cajeta kernel`

Cajeta runs as a Jupyter kernel. One kernel process hosts one persistent
JIT session; each cell compiles into that session and can see everything
the cells before it declared. Bindings, types, and top-level methods
persist across cells; output streams while a cell runs; a cell that
throws does not take the kernel with it.

Works with Jupyter Lab, classic Notebook, `jupyter console`, VSCode's
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

It will not overwrite an existing spec — pass `--force` if you mean to
replace one (after moving the binary, say).

Then `jupyter lab` and pick **Cajeta** from the launcher.

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

A cell is a *script unit*: top-level statements, methods, and type
declarations freely mixed, with no enclosing class. The language rules
are in `specs/script-units-spec.md`; the short version:

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

**A trailing expression is the cell's result** and renders as `Out[N]`.
Primitives and `String` render directly; an object renders through its
`toString()` (including `@ToString`-synthesized ones). A cell ending in
a declaration, a loop, a `return`, or a void call has no result and
displays nothing — and neither does an assignment, since `x = 1` is a
statement, not a value.

When a rendered value is itself JSON — an object or an array — the
result also carries an `application/json` bundle for frontends that
render structured output. Scalars deliberately do not: an
`application/json` of `42` is noise, not capability.

**Redefinition is generational.** Re-declaring a type in a later cell
creates a new generation rather than mutating the old one. Values
created from the earlier generation keep their own layout and methods —
they do not silently reinterpret themselves under the new definition.

## Errors

A cell that fails to compile, and a cell that throws, both come back as
a Jupyter `error` with a type, a message, and a traceback. Traceback
frames name **cells**, not the synthesized classes cells compile into:

```
Exception: boom
  In[4], line 1
```

The session survives either. A failed compile leaves it completely
untouched; an uncaught throw unwinds the cell and leaves the session's
bindings intact.

The exception is an **unrecoverable** error, which is a panic rather
than a cell error: the kernel reports it and exits, because continuing
over a world the runtime has already declared broken is worse than
stopping.

Warnings reach the notebook as `stderr` output. Only the cell's own
diagnostics are shown — the session's first compile pulls the stdlib
through the same machinery, and none of that is yours to read.

## Interrupt

Interrupting from the frontend (the stop button, or `i i` in a
console) stops the running cell at its next statement boundary. The
cell ends as a `KeyboardInterrupt` error; the session and its bindings
survive; the next cell runs normally.

The kernel stays responsive while a cell is stuck — interrupts are
handled off the execution thread, so the control channel answers even
mid-runaway-loop.

**Limitation.** Interruption is safepoint-granular, and safepoints exist
in your cell's code. A cell parked inside a long stdlib call or a native
call will not stop until it returns to its own code. An interrupt sent
while nothing is running is a no-op.

## Restart

Restarting drops every session binding (destructors run), resets the
execution counter to 1, and starts from an empty session. Names bound
before the restart are gone.

## Projects and dependencies

Launch the kernel from inside a project directory and that project's
`cajeta.json` classpath applies, so a notebook can import project
dependencies and Olla libraries exactly as a compiled program would.

## Security

Messages are HMAC-SHA256 signed with the key from the connection file,
per the Jupyter protocol. A message whose signature does not verify is
dropped: not answered, not logged back to the sender, not dispatched.
An empty key is the protocol's explicit unsigned mode.

## Limitations in v1

- No completion or introspection: `Tab` and `?` return empty results
  rather than hanging the frontend, but there is no engine behind them
  yet.
- No `stdin` — a cell cannot prompt for input.
- Rendering is text plus the JSON bundle described above. Images and
  HTML tables are staged behind display protocols in a later revision.
- No debugger bridge yet. Breakpoints, pause, and step inside a
  notebook are the v1.5 layer designed in
  `docs/specification/debugging/Debugging.md`.
