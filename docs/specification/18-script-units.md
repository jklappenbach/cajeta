# 18 — Script Units

This chapter defines script units: compilation units of loose statements and non-class-bound declarations, executed directly by `cajeta run`, with arguments through `System.args`. A script unit is not a dialect — it passes through the same pipeline as every program: statically typed, borrow-checked, monomorphized, JIT-compiled.

## 18.1 The Script Shape

A compilation unit containing at least one loose statement or top-level method takes the script shape. There is no mode flag and no special file extension; a unit made only of type declarations parses as an ordinary compilation unit. Legal at top level: statements; method declarations bound to no class; and type declarations (classes, interfaces, annotations, views), usable by the statements around them.

```cajeta
int64 n = System.args.count();
if (n == 0) { System.stdout.println("usage: greet <name>"); return 2; }
System.stdout.println("hello, " + System.args.get(0));
```

```text
$ cajeta run greet.cajeta world
hello, world
```

**The desugaring model.** A script unit compiles as an implicit final class with an implicit static entry method: the wrapper the author would otherwise write, synthesized. The wrapper is invisible — diagnostics carry the host file and the user's line numbers, and stack traces render script frames as `<script>` (Errors §15.5); the synthesized names never appear.

## 18.2 Session Bindings

Top-level owner declarations bind into the **session scope**, not the entry's frame: they survive the entry's return and drop when the session ends, in reverse binding order — or earlier, when the name is rebound, which drops the old value at the rebind. Block-nested locals inside a script keep ordinary scope-exit drops, and a block-local shadowing a session binding is an ordinary local (Names §7.1).

Two consequences are compile-time errors:

- A top-level binding cannot hold a borrow — the borrowed owner may drop or rebind in a later unit (`CAJETA_ERROR_SESSION_BORROW_ESCAPE`). Transfer instead (`#=`), bind a fresh value, or borrow inside a block.
- A `stack` allocation cannot bind at top level — session bindings outlive the frame (`CAJETA_ERROR_SESSION_STACK_BINDING`; Allocation §4.3).

> *Discussion.* Two session-scope ownership gaps are open as of 0.27.0, recorded as disabled pinning tests in `test/jit/SessionBindingTests.cpp`: the move-of-borrow check does not yet reach top-level `#=` stores (Ownership §5.6), and an array-typed top-level `#=` loses the transferred buffer's contents. Both are in scope of the ownership consolidation under way.

## 18.3 Arguments — `System.args`

Argv arrives through `System.args`, ambient beside `System.env` and `System.property`: a helper any depth below the top level reads it without threading a parameter. `count()` returns `int64`; `get(i)` returns a `String`, or null past the end — the same shape `System.env.get` uses for an unset variable. It is read-only: mutation is a compile-time error (`CAJETA_ERROR_ARGS_READ_ONLY`).

**One store, two spellings.** A class entry declared `static int32 main(String[] args)` receives an array built from the same store `System.args` reads; the two cannot disagree, because the `argv[0]` slicing decision is made once, when the host installs the store. Every host installs it — `cajeta run`, a compiled binary's entry shim, and the notebook kernel, which installs an explicitly empty vector because a cell has no argv (Notebook Kernel §19).

## 18.4 Exit Semantics

A top-level `return <int32>;` ends the unit and, under `cajeta run`, becomes the process exit code; a script without one exits 0. A trailing expression statement is the unit's *result*: the notebook renders it (`Out[N]`), the script host ignores it. An uncaught throw prints the message and a `<script>`-frame trace and exits non-zero.

## 18.5 Dependencies

A script depends on classes, never on other scripts: there is no import of another script's loose declarations, and a script unit's loose declarations are private to it. Dependency resolution is host-independent and manifest-driven — run inside a project (any ancestor directory holding `cajeta.json`), a script gets the manifest's resolved classpath and imports Olla libraries and project classes exactly as a compiled program would; a standalone script sees the standard library. The notebook kernel resolves identically (Notebook Kernel §19). Shared code is factored into a class in a package and added to the manifest or published.

> *Discussion.* Shebang (`#!`) handling and operating-system file association are deferred surface, out of scope for this chapter's version.
