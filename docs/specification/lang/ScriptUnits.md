# Script units

A compilation unit does not need a class. A file containing loose
statements — optionally mixed with method and type declarations — is a
**script unit**, and `cajeta run` executes it directly:

```cajeta
// tool.cajeta
import cajeta.collection.ArrayList;

ArrayList<int32> xs = heap ArrayList<int32>();
xs.add(40);
xs.add(2);

int32 total = sum(xs);
System.stdout.println("total = " + total);

int32 sum(ArrayList<int32> vals) {
    int32 acc = 0;
    for (int32 i = 0; i < vals.count(); i = i + 1) { acc = acc + vals[i]; }
    return acc;
}
```

```
$ cajeta run tool.cajeta
total = 42
```

There is no mode flag or special file extension. A unit with at least one
loose statement or top-level method takes the script shape; a unit made
only of type declarations parses exactly as before. Script units compile
through the same pipeline as everything else — statically typed,
borrow-checked, monomorphized, JIT-compiled — and the same source also
runs as a notebook cell under the Jupyter kernel.

## The model

A script unit is an implicit final class with an implicit static entry
method; the compiler synthesizes the wrapper you would otherwise write by
hand. Loose statements form the entry body in source order. Top-level
methods become static members, hoisted so statements can call a method
declared later in the file. Type declarations are ordinary top-level
types.

With no `package` declaration, the implicit class lands in the reserved
`cajeta.script` package (user code cannot declare it, so collisions are
impossible). A declared package is honored. Reflection sees the implicit
class but marks it synthetic; diagnostics and stack traces never show it —
errors carry the script file and line, and trace frames render as
`<script>`.

## Session bindings

Top-level declarations do not live in the entry's stack frame. They bind
into a **session** whose lifetime the host controls: for `cajeta run` the
session is the program run, for the Jupyter kernel it spans cells. The
memory model applies unchanged — one owner, borrows by default, `#`
transfers, drops in reverse binding order when the session ends.

Three rules follow from the binding outliving the unit's frame:

- **`heap` only at top level.** A `stack` allocation cannot bind to a
  session name (`CAJETA_ERROR_SESSION_STACK_BINDING`); use `heap`, or
  declare inside a `{ }` block for ordinary frame-local lifetime.
- **No top-level borrows.** `Point q = p;` at top level is rejected
  (`CAJETA_ERROR_SESSION_BORROW_ESCAPE`) — the borrowed owner could drop
  or rebind in a later unit while `q` still points at it. Borrow inside a
  block, or transfer ownership (`q #= p`).
- **Rebinding drops the old value.** `xs = heap ArrayList<int32>()` on an
  existing name destroys the previous value at that moment, then owns the
  new one. Redeclaring the name in a later cell does the same
  (last-write-wins).

Moving a session binding away (`sink.take(#xs)`) behaves as it does for
any local within the unit. Across units the rule is stricter: a binding
moved out in one cell is unreadable in later cells until rebound — the
error names the transfer site and the cell that moved it.

Block-nested declarations are ordinary locals: they drop at block exit,
and the session never sees them.

## Exit code and results

An explicit `return <int32>;` at top level ends the unit and, under
`cajeta run`, becomes the process exit code; a script without one exits 0.
A trailing expression statement is the unit *result* — the notebook
renders it (`Out[N]`), the script host ignores it. An uncaught throw
prints the message and a `<script>`-frame trace and exits non-zero.

## Arguments

A script has no parameter list, so argv arrives through `System.args`,
beside the existing `System.env` and `System.property` namespaces:

```cajeta
// greet.cajeta
int64 n = System.args.count();
if (n == 0) { System.stdout.println("usage: greet <name>"); return 2; }
System.stdout.println("hello, " + System.args.get(0));
```

```
$ cajeta run greet.cajeta world
hello, world
```

`count()` returns `int64`; `get(i)` returns a `String`, or **null** past the
end — the same shape `System.env.get` uses for an unset variable, so one
idiom covers both. It is read-only: argv reports how the process was
invoked, and a program that could rewrite it would be lying to anything
that read it afterwards (`CAJETA_ERROR_ARGS_READ_ONLY`).

It is **ambient** on purpose. A helper several frames below the top-level
statements reads argv without it being threaded down, exactly as it reads
the environment — which is the case a `String[] args` parameter serves
badly and the reason the accessor was chosen over injecting a name.

**One store, two spellings.** A class entry declared
`static int32 main(String[] args)` still receives its `String[]`, and that
array is built from the same store `System.args` reads. The two cannot
report different arguments. This matters because the hosts do not agree
naturally: a compiled binary must slice `argv[0]` (the program name) while
the JIT never had it, and when each host marshalled its own vector, one
spelling could disagree with the other about `args[0]`. The slicing
decision now happens once, at install.

Every host installs — `cajeta run`, `cajeta jit-run`, a compiled binary's
`main` shim, and the Jupyter kernel, which installs an EMPTY vector because
a notebook cell has no argv. Stating that explicitly is deliberate: a store
left uninitialized would report zero arguments, which is indistinguishable
from a program invoked with none.

## Projects and dependencies

A script run inside a project — any ancestor directory holding
`cajeta.json` — gets the manifest's resolved dependencies on its
classpath, exactly as `cajeta build` would. A standalone script sees the
stdlib.

Spec: `specs/script-units-spec.md` (design), `specs/jupyter-kernel-spec.md`
(the multi-cell consumer).
