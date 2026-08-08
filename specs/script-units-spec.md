# Script units — top-level code as a language feature

## 1. Definition

**1.1 Purpose.** Cajeta today admits only `packageDeclaration?
importDeclaration* typeDeclaration*` as a compilation unit
(`CajetaParser.g4:36`): the minimal runnable program is a class with an
entry method. This spec adds **script units**: a compilation unit may
instead be a sequence of loose statements, optionally mixed with method
and type declarations. Script units are the language-level foundation for
two consumers: **script files** (`cajeta run tool.cajeta`) and **notebook
cells** (the Jupyter kernel, `jupyter-kernel-spec.md`). The semantics of
top-level code — naming, ownership, lifetime, redefinition — are owned by
the language, here, not by any one consumer.

**1.2 The model in one paragraph.** A script unit IS an implicit class
with an implicit entry method: the compiler synthesizes the wrapper that
a developer writes by hand today. Loose statements form the entry-method
body in source order; top-level method declarations become members of the
implicit class; type declarations are ordinary siblings. Top-level
bindings live in a **session scope** — an enclosing scope whose lifetime
is decided by the host: a script run is a session of one unit (bindings
drop at program exit); a kernel session spans many units (bindings
persist across cells). Ownership, borrowing, `#`-transfer, and the drop
chain apply to session bindings by the existing memory-model rules; the
only new concept is the session scope itself.

**1.3 Constraints.**
- No new execution model: script units compile through the same
  pipeline (parse → Phase 1/2 → LLVM IR → JIT or binary) as ordinary
  units. Monomorphization, the borrow checker, and the drop chain apply
  unchanged.
- Diagnostics must speak the user's language: errors, lints, and stack
  traces reference the script/cell source line, never the synthesized
  wrapper's names.
- A file that parses as an ordinary unit today parses identically
  tomorrow — script units extend the grammar; they change nothing about
  existing programs.

**1.4 Non-goals.**
- No interpreter and no dynamic typing: script units are compiled,
  statically typed, borrow-checked cajeta.
- No REPL-grammar restructuring: the compilation unit remains the unit
  of compilation; hosts feed whole units (a file, a cell).
- Shebang (`#!`) handling, OS file association, and kernel-spec
  installation are host/packaging concerns, out of scope here.
- Top-level `await` is out of scope for v1 (a cell/script body is
  synchronous; `async` code runs via `scope { spawn ... }` as anywhere
  else).

## 2. Grammar and unit shape

Requirements:

- `compilationUnit` additionally admits: `packageDeclaration?
  importDeclaration* scriptMember*` where `scriptMember` is a
  `blockStatement`, a method declaration, or a type declaration, freely
  interleaved. A unit containing at least one loose statement or
  top-level method is a **script unit**; otherwise it is an ordinary
  unit. There is no mode flag, file extension, or marker.
- Loose statements execute in source order as the unit body. Top-level
  method and type declarations are hoisted: they are visible to every
  statement in the unit regardless of position.
- A trailing expression statement is the **unit result** (see 5.6).

Use cases:

- **2.1** When a file contains only `import` lines followed by
  statements, it compiles as a script unit with no class written by the
  developer.
- **2.2** When a unit mixes a class declaration, a top-level method, and
  loose statements, all three coexist: the class is an ordinary type,
  the method is callable from the statements, and the statements run in
  order.
- **2.3** When a statement calls a top-level method declared later in
  the same unit, it resolves (hoisting), matching how methods already
  behave inside a class.
- **2.4** When a unit has no loose statements and no top-level methods,
  it parses exactly as today — existing sources are unaffected.
- **2.5** When a `package` declaration is present in a script unit, the
  implicit class lives in that package; when absent, it lives in a
  reserved script package (see 3.2).
- **2.6** When a construct legal only inside a method body appears at
  top level (e.g. `return` with a value, `break`), it is diagnosed by
  the same rules as in a method body — `return` ends the unit body
  (see 5.6); `break`/`continue` outside a loop remain errors.

## 3. The implicit class and entry

Requirements:

- Each script unit synthesizes one final class and one static entry
  method; the loose statements are the entry body, and top-level
  methods become static members of the class. Bindings are NOT static
  fields — they are session bindings (Section 4).
- Synthesized names are reserved, deterministic, and host-supplied (a
  script host derives them from the file, the kernel from the cell
  number). They never appear in diagnostics or traces (Section 6).

Use cases:

- **3.1** When two top-level methods and the statements reference each
  other, they compile as members of one class — no qualification
  needed.
- **3.2** When no package is declared, the implicit class is placed in a
  reserved package (`cajeta.script`) that user code cannot declare;
  collisions with user types are impossible.
- **3.3** When a script unit declares a type, that type is a normal
  top-level type in the unit's package — other units (later cells, or
  ordinary sources compiled alongside a script) can import and use it.
- **3.4** When reflection walks a script unit's types, the implicit
  class is visible but marked synthetic, so tooling can filter it.

## 4. Session scope and ownership

The session scope is an enclosing scope that outlives any single unit.
Its lifetime is host-defined: `cajeta run` = the program run; the kernel
= the kernel session. Top-level bindings (`var x = …` or typed
declarations at top level) bind into the session scope.

Requirements:

- Session bindings follow the memory model unchanged: one owner,
  borrows by default, `#name` transfers, drops in reverse binding order
  when the session scope exits.
- The ownership state of a session binding spans units: a binding moved
  out in one unit is unreadable in later units until rebound — the same
  `CAJETA_ERROR_*` diagnostics as for locals, reported at the later
  unit's use site.

Use cases:

- **4.1** When unit K binds `var xs = heap ArrayList<int32>()` and unit
  K+1 calls `xs.add(1)`, the binding resolves and the value is alive —
  session bindings persist across units.
- **4.2** When a unit transfers a session binding away
  (`sink.take(#xs)`), a later unit reading `xs` gets the standard
  use-after-move compile error, at that unit's line.
- **4.3** When a session binding is rebound (`xs = heap ArrayList…()`
  in a later unit), the previous value is dropped at the moment of
  rebinding, then the name owns the new value.
- **4.4** When the session ends (script exit, kernel shutdown or
  reset), all session bindings drop in reverse binding order, firing
  destructors exactly as scope exit does today.
- **4.5** When a unit declares a local inside a block (`{ var t = …; }`),
  it is an ordinary local with scope-exit drop — only top-level
  declarations become session bindings.
- **4.6** When a session binding is borrowed by a local within a unit,
  the borrow is checked within that unit; borrows cannot escape a unit
  (a unit ends like a function body — borrow-escape diagnostics apply).
- **4.7** When a `stack` allocation is bound at top level, it is
  rejected with a directive diagnostic: session bindings outlive the
  unit's frame, so only `heap` values (and value copies) can bind at
  top level of a multi-unit session; a single-unit script host MAY
  relax this to plain scope semantics (open question 8.1).

## 5. Multi-unit sessions: visibility and redefinition

These requirements bind when a session contains more than one unit (the
kernel). A script run is the one-unit degenerate case and is unaffected.

Requirements:

- Later units see earlier units' session bindings, top-level methods,
  and types — the session is one accumulating namespace.
- Redefinition is **last-write-wins for bindings and methods**, and
  **generational for types**: redefining a class introduces a new type
  generation. Existing values of the old generation remain alive and
  usable through bindings already typed to it; the class *name* resolves
  to the newest generation in later units. Old and new generations are
  distinct types — passing an old-generation value where the new
  generation is expected is an ordinary type error, diagnosed with a
  "stale generation" hint.
- A body-only method change (same signature) replaces the
  implementation in place — callers compiled earlier call the new body.
  (This is the same compatibility class as hot-reload rule 2 in
  `Debugging.md`; the two features share the definition of
  "body-only".)

Use cases:

- **5.1** When cell 2 calls a method defined in cell 1, it resolves and
  runs — no import or qualification.
- **5.2** When cell 3 redefines `value()` and cell 4 calls it, cell 4
  gets the new body; a direct call after redefinition never sees the
  old one.
- **5.3** When cell 5 redefines `class Point` with a different layout,
  a `Point` bound in cell 2 stays alive and its methods still run
  (old generation); `Point` in cell 6 means the new generation; mixing
  them is a compile-time type error naming both generations.
- **5.4** When cell 5's redefinition of a class is body-only (method
  bodies changed, layout and signatures identical), no new generation
  is introduced — existing values adopt the new bodies.
- **5.5** When a unit fails to compile, the session is unchanged: no
  binding, method, or type from the failed unit is visible afterward.
- **5.6** When a unit ends with an expression statement, its value is
  the unit result: the kernel renders it (`Out[N]`), a script host
  ignores it (script exit code comes from an explicit `return int32`,
  else 0).

## 6. Diagnostics, traces, and tooling

Requirements:

- Compile diagnostics for a script unit carry the host's source name
  (file path, or cell id) and the user's line/column — the synthesized
  wrapper is invisible.
- Runtime stack traces render script frames as `<script>` /
  `In[N]` with the user line, not the synthesized class/method names.
- Lints apply to script units; lints that are wrapper artifacts (e.g.
  unused-variable for a binding the *session* still holds) must not
  fire falsely.

Use cases:

- **6.1** When a type error occurs on line 3 of a cell, the diagnostic
  says line 3 of that cell — not a line inside a synthetic class.
- **6.2** When a script throws and the trace is dumped, frames inside
  the script body show the script file and line; synthesized frames are
  renamed, never `Cell$7.__cell$7`.
- **6.3** When the IDE or LSP opens a script file, completion and
  navigation work against the implicit-class view (top-level methods
  and bindings resolve as members/locals).

## 7. Script execution — `cajeta run`

Requirements:

- `cajeta run <file>.cajeta` compiles the file as a script unit and
  executes it as a one-unit session under the JIT (the cached-JIT
  write-once/run-anywhere path); `--emit=exe` of a script unit produces
  a native binary with the same semantics.
- A script runs either standalone (stdlib only) or inside a project:
  when a `cajeta.json` governs the file, its dependencies are on the
  classpath.

Use cases:

- **7.1** When `cajeta run hello.cajeta` executes a three-line script,
  it compiles, runs, drops session bindings at exit, and exits 0.
- **7.2** When the script's trailing `return 3;` executes, the process
  exit code is 3.
- **7.3** When a script is run inside a project directory, `import`s of
  project dependencies resolve exactly as they would in `src/`.
- **7.4** When a script throws an uncaught recoverable exception, the
  message and trace print (per 6.2) and the exit code is non-zero.

## 8. Open questions

- **8.1** Top-level `stack` bindings in a single-unit script: relax 4.7
  (treat the script body as one frame) or keep the uniform rule?
  Recommendation: keep uniform (heap-only at top level) for one rule
  everywhere; revisit if script ergonomics demand it.
- **8.2** Should the unit result (5.6) of a script be usable as the
  exit code when it is `int32` and no explicit return exists?
  Recommendation: no — explicit `return` only; silent value-to-exit
  coupling surprises.
- **8.3** Reserved script package name: `cajeta.script` vs `__script`.
  Recommendation: `cajeta.script` (pronounceable, filterable, already
  in the reserved `cajeta.*` namespace).
