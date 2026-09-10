# 18 — Script Units

Have you ever wanted to write a script in Java? Probably not. Yet scripting is constant work, and it gets done in JavaScript, Python, and Ruby. The difference is not what those languages can compute. It is what they ask for before they compute anything.

**What the Java model asks for.** A program begins with a class, and inside it a static entry method with a fixed signature. A three-line task arrives wrapped in two layers of declaration that say nothing about the task. Execution is structured all the way down, so a statement cannot stand at the top of a file and a sequence of steps has nowhere to live. Between the file and the result sits a build step. An `import` resolves only after a build tool has been told about the dependency, which means a project before a script.

**What this chapter defines.** Script units remove each of those without introducing a second language. A script unit is not a dialect. It passes through the same pipeline as every other program — statically typed, borrow-checked, monomorphized, JIT-compiled.

- Statements are legal at the top level of a compilation unit, alongside methods and types that belong to no class. The class and entry method are synthesized, and never appear in a diagnostic or a stack trace (§18.1).
- `cajeta run greet.cajeta` compiles and runs the file. There is no build step to invoke and no artifact to name.
- A name bound at the top level lives in the session rather than the entry frame, so a later unit still sees it (§18.2).
- Arguments arrive ambiently through `System.args` (§18.3), and a top-level `return` is the process exit code (§18.4).

**Import, then provision.** A scripting language's `import` assumes that someone already installed the library. `pip install`, `npm install`, and `bundle install` are separate acts performed against an ambient environment, and a script that arrives on another machine fails at its first import until they are repeated there. A Cajeta script imports what its project declares, and running it provisions what it declares — the repository list is queried, artifacts are fetched into the local store, and the resolved graph is recorded (§18.5). The toolchain is pinned the same way, and a mismatched one is fetched and dispatched into rather than reported as a version error. Declaring a dependency and having it is one step, not two.

## 18.1 Top-Level Code

A compilation unit containing at least one loose statement or top-level method is a script unit. There is no mode flag and no special file extension, and a unit made only of type declarations parses as an ordinary compilation unit. Three things are legal at top level — statements, method declarations bound to no class, and type declarations (classes, interfaces, annotations, views) usable by the statements around them.

```cajeta
int64 n = System.args.count();
if (n == 0) { System.stdout.println("usage: greet <name>"); return 2; }
System.stdout.println("hello, " + System.args.get(0));
```

```text
$ cajeta run greet.cajeta world
hello, world
```

**The desugaring model.** A script unit compiles as an implicit final class with an implicit static entry method — the wrapper the author would otherwise write, synthesized. The wrapper is invisible — diagnostics carry the host file and the user's line numbers, and stack traces render script frames as `<script>` (Errors §15.5). The synthesized names never appear.

## 18.2 Session Bindings

Top-level owner declarations bind into the **session scope**, not the entry's frame. They survive the entry's return and drop when the session ends, in reverse binding order — or earlier, when the name is rebound, which drops the old value at the rebind. Block-nested locals inside a script keep ordinary scope-exit drops, and a block-local shadowing a session binding is an ordinary local (Names §7.1).

Two consequences are compile-time errors:

- A top-level binding cannot hold a borrow — the borrowed owner may drop or rebind in a later unit (`CAJETA_ERROR_SESSION_BORROW_ESCAPE`). Transfer instead (`#=`), bind a fresh value, or borrow inside a block.
- A `stack` allocation cannot bind at top level — session bindings outlive the frame (`CAJETA_ERROR_SESSION_STACK_BINDING`, Allocation §4.3).

> *Discussion.* Two session-scope ownership gaps are open as of 0.27.0, recorded as disabled pinning tests in `test/jit/SessionBindingTests.cpp`: the move-of-borrow check does not yet reach top-level `#=` stores (Ownership §5.6), and an array-typed top-level `#=` loses the transferred buffer's contents. Both are in scope of the ownership consolidation under way.

## 18.3 Arguments — `System.args`

Argv arrives through `System.args`, ambient beside `System.env` and `System.property`, so a helper any depth below the top level reads it without threading a parameter. `count()` returns `int64`, and `get(i)` returns a `String`, or null past the end — the same convention `System.env.get` uses for an unset variable. It is read-only, and mutation is a compile-time error (`CAJETA_ERROR_ARGS_READ_ONLY`).

**One store, two spellings.** A class entry declared `static int32 main(String[] args)` receives an array built from the same store `System.args` reads. The two cannot disagree, because the `argv[0]` slicing decision is made once, when the host installs the store. Every host installs it — `cajeta run`, a compiled binary's entry shim, and the notebook kernel, which installs an explicitly empty vector because a cell has no argv (Notebook Kernel §19).

## 18.4 Exit Semantics

A top-level `return <int32>;` ends the unit and, under `cajeta run`, becomes the process exit code, and a script without one exits 0. A trailing expression statement is the unit's *result* — the notebook renders it (`Out[N]`), and the script host ignores it. An uncaught throw prints the message and a `<script>`-frame trace and exits non-zero.

## 18.5 Dependencies

A script depends on classes, never on other scripts. There is no import of another script's loose declarations, and a script unit's loose declarations are private to it. Dependency resolution is host-independent and manifest-driven — run inside a project (any ancestor directory holding `cajeta.json`), a script gets the manifest's resolved classpath and imports Olla libraries and project classes exactly as a compiled program would. A standalone script sees the standard library. The notebook kernel resolves identically (Notebook Kernel §19). Shared code is factored into a class in a package and added to the manifest or published.

**Provisioning.** Resolution is not a lookup against whatever happens to be installed. The manifest's repository list is queried in priority order, artifacts are fetched into the machine-local store, version conflicts are settled by minimum-version selection, and the resolved graph is written to a lockfile, so the same script resolves the same way on the next machine. The toolchain is pinned in the same manifest, and a running toolchain that does not match the pin is fetched and dispatched into rather than reported as a version error. The mechanics are the build tool's and are specified in its own document. The consequence for a script is what this chapter states — a declared dependency is a present dependency, with no install step in between.

> *Discussion.* Shebang (`#!`) handling and operating-system file association are deferred surface, out of scope for this chapter's version.
