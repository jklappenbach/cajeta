# external-debug — spec

Status: draft (2026-07-12). Debugging an **AOT** Cajeta binary from an external
debugger (gdb), using Cajeta's own line/type encoding rather than DWARF.

## 1. Definition

### 1.1 Purpose
Make a native `cajeta build` binary debuggable: breakpoints on a source line,
a stack that reads in Cajeta terms, and locals rendered with their names, types,
values, and ownership. Do it with the encoding the language already emits, so
one mechanism serves the whole product matrix.

### 1.2 The problem
Today an AOT binary carries no debug information of any kind. gdb can break on a
mangled symbol and unwind function names; it cannot show a source line, a
variable, or a Cajeta-level frame. Verified 2026-07-11 against `samples/tour`:
`break main` stops, `bt` shows symbols, and `info locals` / `info args` answer
`No symbol table info available`. There are zero `.debug_*` sections.

The machinery to fix this mostly exists, but is unreachable from an AOT binary:

- **Safepoints and locals** — codegen emits `__cajeta_dbg_safepoint(loc_id)` per
  statement and `__cajeta_dbg_local(name, type, addr, alloc, ownership,
  drop_entry)` per local, gated on `flags.debugInfo`. That flag can only be set
  by the JIT host: `--debug-info` is not a CLI flag (the compiler rejects it),
  and the debug flavor's `debug-info` property maps to an empty compiler flag.
  An AOT binary therefore contains neither.
- **The location table** — `DbgLocTable` maps `loc_id → {file, line, col,
  function}` and answers `idsForLine(file, line)`. It is a compiler-process
  global. It works for `cajeta dap` only because the DAP compiles and runs in one
  process. It is never written into a binary, so an external debugger cannot map
  a safepoint to a source line, nor a source line to a breakpoint.
- **Type metadata** — every class's RTTI already carries field names, byte
  offsets, type flags, modifiers, and instance size, reachable from any object
  through its vtable. But RTTI emission is demand-driven (`ReflectionKeep`), so a
  build that never reflects may carry none.
- **The shadow stack** — `__cajeta_line_enter/mark/leave` + a per-method
  `#FrameDesc` already resolve a *captured* trace. Nothing exposed the *live*
  stack a stopped debugger needs; `__cajeta_print_stack` and the frame accessors
  were added 2026-07-11 to close that, and gdb can now render a semantic stack.

### 1.3 Why not DWARF
DWARF is host-only in practice and cannot express what this language most needs
shown: ownership, allocation kind, and drop state. The Cajeta encoding is already
emitted, already works under the JIT and on device targets, and resolves the
**dynamic** type of a value from its vtable rather than the static declared type.
One encoding serves every target; a DWARF path would be a second mechanism to
keep in sync, for host only.

### 1.4 Constraints
1.4.1 No `.debug_*` sections. No `DIBuilder`.
1.4.2 The encoding must be identical under JIT, AOT, and device targets. Anything
      an external debugger reads, `cajeta dap` must be able to read the same way.
1.4.3 Debug data is opt-in and tiered. A default `release` build pays nothing.
1.4.4 Debug emission must not change program semantics — only add records.

### 1.5 Non-goals
1.5.1 gdb's native verbs (`break File:42`, `step`, `info locals`). Those are
      DWARF-driven inside gdb. This spec delivers `cj*` commands instead.
1.5.2 Line-tables-only DWARF as a host complement. Considered and declined
      (2026-07-12); revisit only if native verbs become a requirement.
1.5.3 Editing values from gdb. Read-only inspection in this cut.
1.5.4 Debugging optimized (`release`) builds. Debug data is emitted for
      `--debug-info=line|full`; inlining/reordering under `release` is out of
      scope.

## 2. Debug levels and flag plumbing

The single switch controlling what a build carries.

### 2.1 Requirements
2.1.1 `--debug-info=off|line|full` is a compiler frontend flag.
      - `off` — nothing (today's behavior).
      - `line` — shadow stack + `#FrameDesc` (already default-on via
        `--line-info`), no safepoints, no local records, no forced RTTI.
      - `full` — adds `__cajeta_dbg_safepoint`, `__cajeta_dbg_local`, the
        embedded location table (§3), and forces RTTI retention (§4).
2.1.2 The build flavor's existing `debug-info: off|line|full` property lowers to
      that flag. It currently maps to an empty compiler flag and is dropped.
2.1.3 Flavor defaults: `debug` → `full`, `release` → `line`.
      Revised 2026-07-12 (was `release` → `off`): the built-in release flavor
      has always carried `debug-info: line`, and today's release codegen — with
      the property silently dropped — is exactly `line` (shadow stack on,
      no safepoints). Lowering `off` instead would strip the shadow stack and
      change release output, contradicting 2.2.2 and 7.2. A release build that
      wants nothing still asks for it: `--debug-info=off`.
2.1.4 The level is recorded in the build's flag set so an incremental cache
      never serves a `full` artifact to an `off` build or vice versa.

### 2.2 Use cases
2.2.1 As a developer, when I run `cajeta build` with the default debug flavor,
      then the binary carries safepoints, local records, and a location table.
2.2.2 As a release engineer, when I build `--flavor=release`, then the binary
      carries no safepoints or local records and is byte-identical to today's
      release output (i.e. `line`, per 2.1.3).
2.2.3 As a developer wanting cheap semantic traces without debugger overhead,
      when I build `--debug-info=line`, then exception traces still resolve to
      `Type.method(File.cajeta:NN)` and no safepoints are emitted.
2.2.4 As a build-cache user, when I flip `--debug-info`, then the cache treats it
      as a different build and recompiles rather than re-publishing.

## 3. The embedded location table

The compiler's `DbgLocTable`, written into the binary so an external process can
read it.

### 3.1 Requirements
3.1.1 Under `--debug-info=full`, emit the table as a retained, read-only data
      section: for each `loc_id`, its file, line, column, and enclosing function.
3.1.2 Export runtime accessors, `used`/`retain`-marked (nothing in generated code
      calls them; DCE and `--gc-sections` would otherwise drop them):
      `__cajeta_dbg_loc_count()`, `__cajeta_dbg_loc_file(id)`,
      `__cajeta_dbg_loc_line(id)`, `__cajeta_dbg_loc_func(id)`, and
      `__cajeta_dbg_ids_for_line(file, line, out, max)`.
3.1.3 File names in the table are the remapped (build-root-independent) path, so
      the table is reproducible across build roots.
3.1.4 The table must round-trip: an id emitted by codegen resolves to the same
      `(file, line)` the compiler recorded.

### 3.2 Use cases
3.2.1 As a debugger, when I am stopped at a safepoint with `loc_id = N`, then I
      can resolve N to `File.cajeta:LINE` with no compiler present.
3.2.2 As a debugger, when the user asks to break at `File.cajeta:42`, then I can
      resolve that to the set of `loc_id`s to arm.
3.2.3 As a build-reproducibility check, when the same source is built from two
      different roots, then the embedded tables are byte-identical.

## 4. Locals via reflection metadata

Rendering a variable without a type database.

### 4.1 Requirements
4.1.1 Under `--debug-info=full`, force RTTI retention for all types
      (`ReflectionKeep.forcesAll`, reason `debug-info=full`), so field metadata is
      present for any type a local can hold.
4.1.2 Export `__cajeta_rtti_of(obj)` — the object → RTTI step exists internally
      (`cajeta_rtti_from_obj`) but is `static`.
4.1.3 A local is rendered from its debug-frame record (name, static type, address,
      allocation kind, ownership, drop entry) plus, for reference types, the RTTI
      reached through the object's vtable — so the **dynamic** type is what gets
      shown.
4.1.4 Field decoding uses the RTTI's `byteOffset` and `typeFlags` (size,
      int-vs-float, signedness, primitive-vs-reference). No separate type table.
4.1.5 The walk is recursive over reference fields, with a visited set (cycles) and
      a depth cap.
4.1.6 `String` and array locals render their contents, not just an address.
4.1.7 Ownership, allocation kind, and drop state are shown for every local. This
      is the information DWARF cannot express and the reason for the design.
4.1.8 Static fields (`byteOffset == -1`) live in globals; out of scope for this
      cut, and must be reported as such rather than mis-decoded.

### 4.2 Use cases
4.2.1 As a developer stopped at a breakpoint, when I ask for locals, then I see
      each local's name, declared type, value, allocation kind, and ownership.
4.2.2 As a developer inspecting a `Shape s` that actually holds a `Circle`, when I
      render it, then I see `Circle` and `Circle`'s fields.
4.2.3 As a developer inspecting an object graph with a cycle, when I render it,
      then the walk terminates and marks the revisited node.
4.2.4 As a developer debugging ownership, when I inspect a moved-from local, then
      its ownership/drop state says so rather than showing a live value.

## 5. The gdb bridge

The user-facing surface.

### 5.1 Requirements
5.1.1 Ship a gdb Python script with the toolchain, auto-loadable for a Cajeta
      binary.
5.1.2 Commands:
      - `cjstack` — the live semantic stack (`__cajeta_print_stack`; shipped).
      - `cjbreak File.cajeta:NN` — resolve ids via `__cajeta_dbg_ids_for_line`,
        arm a conditional breakpoint on `__cajeta_dbg_safepoint` matching the
        loc_id argument.
      - `cjlocals [depth]` — render the current frame's locals (§4).
      - `cjstep` / `cjnext` — run to the next safepoint / the next safepoint in
        the current frame.
      - `cjlist` — print source around the current line, from the table's path.
5.1.3 Each command fails with a clear message when the binary lacks the data
      (`--debug-info=off`, or a `line`-only build asked for locals).
5.1.4 Breaking at a function's entry symbol shows callers but not the current
      frame, because the prologue's `__cajeta_line_enter` has not run. `cjbreak`
      must arm past the prologue so the current frame is present.

### 5.2 Use cases
5.2.1 As a developer, when I run `cjbreak Tour.cajeta:133` and `run`, then the
      program stops at that line and `cjstack` names it as the top frame.
5.2.2 As a developer, when I `cjstep`, then execution advances one Cajeta
      statement, not one machine instruction.
5.2.3 As a developer on a release binary, when I run `cjlocals`, then I get told
      the binary carries no debug records and how to rebuild.

## 6. Frame source-file correctness

A defect in the existing encoding, fixed here because it corrupts every stack
trace, not only a debugger's.

### 6.1 Requirements
6.1.1 A stdlib frame currently renders as
      `cajeta.lang.stream.Stream<int32>.forEach(:268)` — no file. `FrameDesc`'s
      file comes from `module->remappedSourcePath()`, and the whole stdlib is
      parsed into a single module whose source path is empty.
6.1.2 Record the declaring source file per class (or per method) at parse time,
      and emit `FrameDesc` from that rather than from the module.
6.1.3 Applies to captured exception traces and to the live stack equally.
6.1.4 The same empty-source-path defect reaches `--classpath` dependencies, and
      costs more than rendering there. `ingestClasspath` builds one module per
      archived class through the synthetic ctor, leaving `sourcePath` empty, so
      every dependency module is INDISTINGUISHABLE by the identity the JIT's
      debug loc-id registry keys on. They shared one id range and overwrote each
      other's loc-table entries: exactly one dependency class resolved, and
      breakpoints in any other could not match at all. Each classpath module
      carries its archive-relative entry name as its source path — already
      machine-independent, so reproducibility (§3.1.3) holds.

### 6.2 Use cases
6.2.1 As a developer reading a production exception trace, when it crosses a
      stdlib frame, then that frame names its file and line.
6.2.2 As a developer, when a stack crosses user and stdlib code, then every frame
      is rendered in the same form.
6.2.3 As a developer stepping into a dependency, when the stack crosses several
      classes of one archive, then each frame names ITS own file and line —
      not whichever class of that archive happened to be written last.
6.2.4 As a developer, when I set a breakpoint in a dependency's source, then it
      binds and stops, for every class in the archive rather than one of them.

## 7. Acceptance

7.1 `samples/tour` builds with the default debug flavor and, under gdb with the
    bridge loaded: `cjbreak Tour.cajeta:<line>` stops there; `cjstack` shows the
    Cajeta frames with files and lines; `cjlocals` shows the frame's locals with
    values and ownership; `cjstep` advances one statement.
7.2 A `release` build is unchanged from today (no records, no size regression).
7.3 No `.debug_*` section appears in any artifact.
7.4 `cajeta dap` continues to pass its integration suite, reading the same
    encoding.
7.5 Binary-size delta for `--debug-info=line` and `=full` is measured and
    reported on `samples/tour`; `full` is expected to be large (RTTI keep-all) and
    is accepted on review rather than against a fixed threshold.
