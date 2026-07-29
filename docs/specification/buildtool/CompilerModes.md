# CompilerModes

Specification for cajeta's two-flavor compilation model: **debug
mode** (maximum diagnostics, hand-holding for newcomers, runtime
visibility) vs **release mode** (maximum throughput, minimum
footprint, zero overhead for safety nets we can prove correct).

This doc is the contract for what each mode *means* — what
invariants the user can rely on, what costs they're paying, and
which individual features can be toggled. The low-level flag
plumbing (how flavors expand into compiler flags, the order of
optimization passes, etc.) lives in `Compilation.md`; this doc
defines the *semantic* envelope each flavor inhabits.

## Table of contents

1. [Philosophy](#philosophy)
2. [Mode selection](#mode-selection)
3. [Mode profiles](#mode-profiles)
4. [Feature catalog](#feature-catalog)
5. [Source-tagged drop-chain entries (debug feature #1)](#source-tagged-drop-chain-entries-debug-feature-1)
6. [Mode-conditional codegen](#mode-conditional-codegen)
7. [Phasing](#phasing)
8. [Open questions](#open-questions)

---

## Philosophy

Two audiences, two postures:

- **Debug mode.** Caress the new user. Every runtime failure should
  point at the source line that caused it. Every drop, every
  borrow, every dangling reference, every overflow comes with
  source positions, a clear English description, and a suggested
  fix where one exists. Performance is irrelevant; the user is
  diagnosing, not shipping. Memory cost is fine — debug builds run
  on developer machines.

- **Release mode.** Maximize throughput and minimize footprint.
  Strip everything that's not load-bearing for correctness once
  the program is known to work. Trust the compiler's static
  analysis where it's sound (no use-after-move tracking at
  runtime); rely on the OS / glibc / hardware for the rest (no
  poison-on-free, no live-set claim, etc.). Crashes are
  acceptable diagnostics in production — the production team has
  cores + symbol files + stacktraces from another layer (Sentry,
  glog, etc.) and doesn't need the language to also hold their
  hand.

**No mode is ever a correctness gate.** Code that runs correctly
in debug must run correctly in release. The only differences are
diagnostic surface, runtime checks that the compiler couldn't
prove safe to elide, and source position metadata. If something
behaves differently across modes, it's a bug in mode handling,
not a feature.

**Every debug feature has a toggle.** Users who want one specific
debug feature in production (or want to skip one specific feature
in development) reach for `--<feature>=on|off`. The flavor flags
(`--debug`, `--release`) are conveniences that expand into
feature toggle combinations; individual toggles override.

---

## Mode selection

The **compiler binary** (`src/main.cpp`) supports two layers, applied
left-to-right so later args override earlier ones:

1. Flavor selection: `--mode=debug|debug-release|release|fast|minimal`,
   or its alias flags `--debug` / `--debug-release` / `--release` /
   `--fast` / `--minimal`. This expands to a `CompilerFlags` struct via
   `CompilerFlags::defaultsForMode` (`src/cajeta/compile/CompilerMode.h`).
2. Per-feature overrides: `--source-tags=on`, `--bounds=off`, etc.,
   each mutating one field of that struct after expansion.

Built-in default when no flavor flag is given: `debug` — we don't want
a new user to ship a stripped binary by accident.

> The compiler's own `--profile=<name>` flag is **not** a mode
> selector — it sets the active `@Profile` for component gating.
> Build-flavor *profiles* keyed in `cajeta.json` (a build-tool concept,
> `[profile.<name>]`-style, and any `CAJETA_MODE`-style env selection)
> are resolved by the build tool, which then passes the chosen
> `--mode`/`--<feature>` flags down to the compiler. See
> [`BuildTool.md`](BuildTool.md).

Example (compiler binary):

```sh
cajeta --release    demo.App.run src build/stdlib   # release flavor
cajeta --release --source-tags=on  demo.App.run src build/stdlib
                                                    # release + one debug feature opted in
cajeta --debug   --bounds=off      demo.App.run src build/stdlib
                                                    # debug + one feature opted out
```

---

## Mode profiles

The flavor flags expand into feature-toggle combinations.
Individual flags override.

| Feature                 | `--debug`  | `--debug-release` | `--release` | `--fast`   | `--minimal` |
|-------------------------|-----------|-------------------|-------------|-----------|-------------|
| `--bounds`              | on        | on                | on          | off       | off         |
| `--null-checks`         | on        | on                | on          | on        | off         |
| `--source-tags`         | on        | on                | off         | off       | off         |
| `--poison-free`         | on        | off               | off         | off       | off         |
| `--live-set`            | strict    | strict            | bounded     | bounded   | off         |
| `--drop-chain-validate` | on        | on                | off         | off       | off         |
| `--ub-traps`            | on        | on                | off         | off       | off         |
| `--use-after-move-rt`   | on        | on                | off         | off       | off         |
| `--overflow-checks`     | on        | on                | wrapping    | wrapping  | wrapping    |
| `--stack-trace-capture` | on        | on                | off         | off       | off         |
| `--diag-verbosity`      | verbose   | verbose           | normal      | normal    | terse       |
| `--diag-hints`          | on        | on                | off         | off       | off         |
| `--profile-counters`    | off       | on                | off         | off       | off         |

The IR optimization level (`--opt=O0|O1|O2|O3`) is a separate axis;
each mode picks a default (`O0` for debug, `O2` for release /
debug-release, `O3` for fast — see `CompilerFlags::defaultsForMode`).
Debug-info emission is a mode-driven internal toggle. LTO, PGO, and
frame-pointer flags are planned (see
[`Compilation.md`](Compilation.md#optimization)); they would layer on
top of the feature toggles here.

---

## Feature catalog

Each feature: what it does, its debug + release defaults, its
toggle flag, and the runtime cost paid when enabled.

### `--bounds=on|off|trap`

Array bounds-check generation. `on` emits a comparison + branch
before every array index access; out-of-bounds raises
`IndexOutOfBoundsException`. `trap` skips the exception (no
unwind, no message) and emits an LLVM `@llvm.trap` for the
fastest possible bail-out (still SIGILL, not silent UB). `off`
elides the check.

- Debug default: `on`. Most user-visible source of "why did my
  program crash?" — keep it loud.
- Release default: `on`. Bounds violations are real bugs even in
  production; the cost (a compare + predictable branch) is
  amortized below the cache-line cost of the read anyway.
- Cost: one CMP + conditional branch per index access. Branch
  predictor handles in-bounds (the overwhelming common case)
  near-free.
- Per `docs/specification/lang/MemoryModel.md` § Bounds, the static analyzer
  can elide the check when the index is provably in-range
  (constant, prior compare); both modes use this.

### `--null-checks=on|off|trap`

Null-receiver checks before virtual dispatch + field load. `on`
raises `NullPointerException`; `trap` branches to `@llvm.trap`
(SIGILL, no unwind) like `--bounds=trap`; `off` elides the check.

- Debug default: `on`.
- Release default: `on`. Same logic as `--bounds`.
- Off-default lives in `--minimal` where you've accepted that
  null receivers SEGV.
- Cost: one CMP per virtual call site that the compiler can't
  prove non-null. Often elidable post-allocation site.

### `--source-tags=on|off`

Carry per-allocation source positions (file + line) on every
drop-chain entry and every live-allocation set record.
Diagnostics ("double-free attempted", "drop-chain assertion
failed") then print the alloc-site AND drop-site, not just an
address. **First debug feature; see § Source-tagged drop-chain
entries below for the design.**

- Debug default: `on`.
- Release default: `off`.
- Cost in debug: 16-24 extra bytes per chain entry (alloc-site +
  drop-site, file pointer + line). One extra arg per
  `__cajeta_drop_push` call (passed at codegen time). For typical
  programs with thousands of live drop entries, ~few hundred KB.
- Cost in release: zero (extra fields aren't in the struct, the
  debug-flavored helpers aren't linked).

### `--poison-free=on|off`

After every heap free, scribble the freed body with a recognizable
sentinel (`0xDEADBEEF` for words, `0xDB` for bytes — pick one).
Use-after-free reads then return garbage instead of stale-but-
plausible data, and the sentinel is fast to recognize in a core
dump.

- Debug default: `on`.
- Release default: `off`. Doubles the memory bandwidth on every
  free; not worth it in production.
- Cost: one memset per `free()`. Width = allocation size.
- Optional escalation: track a "should-be-poisoned" flag in the
  live-set and assert on read (via a runtime `__cajeta_load_check`
  helper) — but that requires every field load to gate on the
  helper. Defer to a later doc.

### `--live-set=strict|bounded|off`

The per-thread live-allocation set from `docs/specification/lang/FieldOwnership.md`
§ Solution B. Three states:

- `strict` (debug): unbounded growth + rehash when load passes
  threshold. Never silently stops tracking. Asserts on suspicious
  states (e.g., adding an already-present address).
- `bounded` (release): fixed-capacity table (current 64K slots).
  Over the cap, warn once and stop tracking; subsequent double-
  frees in that regime fall through to glibc's heap checker.
- `off` (minimal): the set isn't allocated, the auto-drop helpers
  don't check it, and free is unconditional. Aliased fields will
  double-free; the user has guaranteed via reasoning that no
  aliased fields exist (or has accepted the cost of glibc abort
  in production).
- Cost (strict / bounded): one `pthread_mutex_lock` + hash op +
  unlock per alloc and free. Single global bottleneck — see
  `docs/specification/lang/FieldOwnership.md` § Cost. Strict additionally
  pays rehash cost amortized across allocations.
- Cost (off): zero. But you'd better know what you're doing.

### `--drop-chain-validate=on|off`

Validate the drop chain at every push and pop: linked-list
integrity check (prev pointer != obj address, no cycle, active
flag is 0 or 1, drop_fn is non-null when active). Cheap O(1)
checks. On failure, log the entry's source tags and abort.

- Debug default: `on`.
- Release default: `off`.
- Cost: ~3 conditional branches per push/pop. Mostly free
  (predictable branches).

### `--ub-traps=on|off`

Generate `@llvm.assume` / explicit trap instructions for
operations the compiler knows are UB if their preconditions are
violated (signed overflow, shift-by-≥-width, divide by zero,
unaligned atomic). In release the compiler trusts the UB and
optimizes around it; in debug the trap fires before the
optimizer would have wrong-coded.

- Debug default: `on`. Catches the "compiler made my code do
  something weird" mystery early.
- Release default: `off`. Trust the spec.
- Cost: per operation, one CMP + branch.

### `--use-after-move-rt=on|off`

The compile-time use-after-move tracker (`MemoryModel.md` § Path-
based borrow tracking) catches most of these. Runtime backup:
mark moved slots with a sentinel (`MOVED_OUT_BIT` in the
slot's header word) and trap if anything reads through them.
Catches indirection cases the static checker misses (e.g., moves
through a stored function pointer).

- Debug default: `on`.
- Release default: `off`. Static check is trusted.
- Cost: one CMP per read from a tracked slot.

### `--overflow-checks=on|off|wrapping`

Integer overflow behavior. `on` traps; `wrapping` is two's-
complement modular arithmetic; `off` is undefined behavior (the
compiler may assume no overflow and optimize accordingly).

- Debug default: `on`.
- Release default: `wrapping`. Trapping in production is a
  reliability risk (single-bad-arithmetic kills the process);
  wrapping is well-defined.
- Cost: one branch per arithmetic op (compiler combines for
  fused checks).

### `--stack-trace-capture=on|off`

When an exception is thrown, capture the fiber's stack via
`backtrace(3)` and attach it to the exception. The system default
catch (`ErrorModel.md` § System default catch) symbolizes the
addresses via DWARF + cajeta source maps and prints
`com.example.Foo::bar (Foo.cajeta:42)` instead of `0x55a3...`.

- Debug default: `on`.
- Release default: `off`. Backtrace is ~1µs per throw; that
  matters in latency-bound code. Tooling like Sentry / glog
  captures stacks at a higher layer for production.
- Cost: ~30 frame walks per throw + symbolization on print.

### `--diag-verbosity=terse|normal|verbose`

Compile-time diagnostic verbosity. `verbose` prints code samples
with the offending span underlined, suggests fixes when one
exists, and links to the relevant doc URL. `normal` is the
default. `terse` is one line per diagnostic for CI parsing.

- Debug default: `verbose`.
- Release default: `normal`.
- Affects the **compiler's** output, not the runtime.
- Cost: build-time only.

### `--diag-hints=on|off`

Whether the compiler offers "did you mean..." suggestions for
typos in identifiers, recommends `#`-transfer when a borrow
violates lifetime, suggests `@SuppressLint("...")` for repeated
ignored warnings, etc.

- Debug default: `on`.
- Release default: `off`. Suppresses noise on CI.
- Affects the compiler's output.

### `--diag-format=text|json`

The **format** of the compiler's diagnostics (independent of `--diag-verbosity`,
which controls their *detail*). `text` is the human-readable default — unchanged
free-text on stderr. `json` emits **NDJSON**: one self-contained JSON object per
line on stderr, so tools consume structured diagnostics instead of regex-scraping
(the IntelliJ plugin and build tool are the consumers; see
`specs/archive/idea-build-toolwindow-spec.md` §1.5.1).

Each line is one diagnostic:

```json
{"severity":"error","code":"CAJETA_ERROR_UNRESOLVED_TYPE","message":"unresolved type 'Foo' in local variable declaration","file":null,"line":null,"column":null}
{"severity":"error","code":"syntax","message":"mismatched input ';' expecting ...","file":"/abs/path/Test.cajeta","line":4,"column":17}
```

- `severity`: `"error"` \| `"warning"` \| `"note"`.
- `code`: the compiler error id (semantic errors) or `"syntax"` (parser/lexer). May be null.
- `message`: the human message. `file` / `line` / `column`: source location when
  known (1-based line/column), else JSON null. Syntax errors carry a precise location;
  **semantic** errors carry one once their site is migrated (the flagship *unresolved
  type* does) — un-migrated sites still report null location.
- **Collect-and-continue** (`--lint` **and full compile**;
  `docs/specs/diagnostic-engine-spec.md`, `docs/specs/collect-continue-compile-spec.md`):
  recoverable semantic errors are *reported to a diagnostic engine and recovered* (the
  failed resolution yields an **error type** so analysis continues) rather than aborting on
  the first. A file surfaces **all** its migrated pre-codegen semantic errors at once,
  sorted by span and deduped. Full compile gates the codegen loop + artifact emit on the
  engine being error-free — a broken tree reports every error and writes **no** artifact;
  a clean tree compiles unchanged. Migration is incremental — an un-migrated site still
  throws and is folded into the engine as one fatal diagnostic.
- Remaining sub-phase (**codegen-in-collect-mode**): run **codegen over error types** with
  per-site **absorption** (an operation on an error type yields an error type, no secondary
  diagnostic) + IR-safe error values, so use-site / type-mismatch / unknown-method errors
  are collected too. It is per-codegen-site work (there is no central assignability
  predicate) and needs IR that survives error subtrees — its own effort.
- Mode-independent (not changed by `--debug`/`--release`/etc.); default `text`.
- A clean compile emits nothing. Consumers should skip any non-`{` line defensively.

### `--lint <file>`

Run the diagnostic passes over **one** source file and report diagnostics, **without
a build** — no codegen, no linking, no artifact, no entry-method. A distinct mode (like
`archive` / `jit-run`): `<file>` is the sole positional; the three-positional
`<entry-method> <source-root> <archive-root>` compile path does not apply.

```
cajeta --lint path/to/File.cajeta                     # text diagnostics
cajeta --lint path/to/File.cajeta --diag-format=json  # NDJSON (IDE editor tier)
cajeta --lint /tmp/buf.cajeta --source-root proj/src --shadow proj/src/app/File.cajeta --diag-format=json
```

- Runs stdlib load → parse → placeholder/prototype/advice/dependency-graph passes, then
  stops before codegen. Surfaces the same diagnostics a full compile's front-end does
  (syntax with precise locations; semantic via error id + message).
- Honors `--diag-format` exactly as a full compile (text default; `json` = NDJSON on
  stderr, nothing on stdout).
- Exit code: `0` iff no error-severity diagnostic; non-zero otherwise. A missing `<file>`
  fails with a clear message, **not** the compile usage banner.
- The IntelliJ plugin's editor-annotation tier (`CajetacRunner`) drives this mode.

**`--source-root <root>`** (optional): resolve `<file>`'s references against the whole
project. Every `.cajeta` under `<root>` is parsed for its **signatures only** (front-end,
no codegen — the same work `--classpath` does for `.cja` deps) and registered as context;
only `<file>`'s diagnostics are reported. A broken sibling is skipped, never aborting or
polluting the linted file. This is what makes `foo.bar()` on a sibling-file type resolve
instead of squiggling. A non-directory `--source-root` fails clearly.

**`--shadow <realpath>`** (optional, with `--source-root`): skip `<realpath>` in the root
walk so the linted `<file>` (a staged, unsaved editor buffer) replaces its on-disk twin —
the edited content is analyzed, with no duplicate-definition clash. A no-op if `<realpath>`
is not under `<root>`.

- **Remaining follow-up (separate effort):** semantic diagnostics are still
  `throw`-based — **single-shot and location-less**, so the linted file still yields one
  semantic diagnostic at no precise offset. Making them *collect-and-continue with spans*
  (multiple, located semantic squigglies) is its own diagnostics-architecture rework
  (see `docs/specs/lint-source-root-spec.md` §1.4). `--source-root` removes false
  *cross-file* positives; it does not change that.

### `--profile-counters=on|off`

Emit per-method invocation counters + per-method total wall-time
tallies (sampled, low-overhead — bumped on entry, time read on
exit, never serialized at runtime; dumped via
`Cajeta.profileSnapshot()`). Used to drive `--pgo=use` in a
later release-flavor build.

- Debug default: `off` (perturbs timing).
- Release default: `off`.
- `--debug-release` default: `on` (the canonical "instrument to
  collect PGO" build).
- Cost: ~2 instructions per method entry; a per-method atomic on
  hot paths can dominate. Use only when collecting profiles.

---

## Source-tagged drop-chain entries (debug feature #1)

The first concrete debug-mode feature. Goal: when ANYTHING goes
wrong with a drop — chain assertion failure, double-free detected
by glibc, poison-byte tripped by a use-after-free read, abort
inside a destructor — the runtime can print exactly which source
allocation and which source drop site are involved.

### Today (release-shape)

```c
struct cajeta_drop_entry {
    void* obj;
    void (*drop_fn)(void*);
    struct cajeta_drop_entry* prev;
    int8_t active;
    /* 7 bytes pad to 32 */
};
```

`__cajeta_drop_push(entry, obj, drop_fn)` records the first
three. Pop runs `drop_fn` if `active` is set.

When something fails, the only thing the runtime can print is
the address. Not useful.

### Debug-shape

```c
struct cajeta_drop_entry_debug {
    void* obj;                      /* +0  */
    void (*drop_fn)(void*);         /* +8  */
    struct cajeta_drop_entry_debug* prev; /* +16 */
    int8_t active;                  /* +24 */
    int8_t _pad[3];                 /* +25 */
    int32_t alloc_line;             /* +28 */
    const char* alloc_file;         /* +32 */
    int32_t drop_line;              /* +40 */
    int32_t _pad2;                  /* +44 */
    const char* drop_file;          /* +48 */
    /* 56 bytes */
};
```

24 extra bytes per entry. Doubles the chain entry storage.

`__cajeta_drop_push_debug(entry, obj, drop_fn, alloc_file,
alloc_line)` records alloc-site at push time.

`__cajeta_drop_pop_run_debug(entry, drop_file, drop_line)`
records drop-site at pop time, then fires drop_fn.

### Compiler-side codegen

Each AST node carries a source position (file, line, column).
The codegen sites for chain pushes are:

- `LocalVariableDeclaration::emitDropEntryFor` — passes the LVD
  expression's file/line.
- `LocalVariableDeclaration::emitDropEntryForFn` — same.
- `Expression.cpp` task-spawn drop push — passes the spawn site.
- Aggregate-init move-deactivation paths — same.

In debug mode each emits `__cajeta_drop_push_debug` with two
extra args; in release mode each emits `__cajeta_drop_push` with
the original three.

For pops, the codegen sites are inside `Statement.cpp` (block-
exit pop emission) and `Expression.cpp` (early-return /
exception-unwind pops). Each passes the closing-brace position
(for block exit) or the return position (for early returns).

### Runtime-side use of the tags

The tags are queried only on failure. Three diagnostic sites:

1. **Live-set assertion** — `live_set_add` on a strict (debug)
   build sees an already-present address. Print:

   ```
   cajeta debug: live-allocation set saw duplicate address 0x55a3...
     First registered at: src/main/cajeta/com/example/Foo.cajeta:42
     This registration:   src/main/cajeta/com/example/Foo.cajeta:68
   ```

2. **Drop-chain integrity check** — `drop_pop_run_debug` checks
   `prev` integrity and active-flag values. On failure:

   ```
   cajeta debug: drop-chain entry corrupted at pop.
     Entry alloc'd at:  src/main/cajeta/com/example/Foo.cajeta:42
     Drop attempted at: src/main/cajeta/com/example/Foo.cajeta:67
     Address:           0x55a3a8...
     Likely cause:      stack frame ran past entry's lifetime.
   ```

3. **Glibc abort handler** — install a SIGABRT handler that
   walks the per-thread chain backwards and prints the head
   entry's tags. Catches the common case of "glibc detected
   heap corruption mid-destructor."

### Storage cost summary

| Mode    | Entry size | Per-1000-entries |
|---------|------------|------------------|
| Release | 32 B       | 32 KB            |
| Debug   | 56 B       | 56 KB            |

Plus the `const char*` table — one entry per source file used,
held as a module-global LLVM constant. Negligible.

### Implementation phasing

1. Add `cajeta_drop_entry_debug` struct to runtime.
2. Add `__cajeta_drop_push_debug` / `__cajeta_drop_pop_run_debug`
   variants.
3. Compiler-side: a single `debugMode` bool on `CajetaModule`,
   reads `--source-tags=on|off` (default per flavor).
4. Each `emitDropEntry*` codegen site picks the variant + struct
   size based on `debugMode`.
5. Add the SIGABRT handler in debug-only runtime init.
6. Tests: a `DebugDiagnosticsTests` suite that intentionally
   triggers each diagnostic site and asserts the printed output
   contains the expected file:line.

---

## Mode-conditional codegen

How the same compiler emits different IR per mode:

- A single `CompilerMode` enum on the top-level `Compiler` (and
  threaded onto `CajetaModule`).
- Per-feature toggles (a struct or hash of bools) computed from
  the flavor flag + per-feature overrides at CLI parse time.
- Codegen sites that vary by mode check the toggle and emit
  different IR / different runtime helper names. Each toggle is
  one branch in the codegen; the emitted IR has zero overhead
  from the toggle itself.
- Runtime helpers come in matched pairs where they differ
  (`__cajeta_drop_push` / `__cajeta_drop_push_debug`,
  `__cajeta_free` / `__cajeta_free_poisoning`). Both live in the
  runtime bitcode; only the one the codegen calls gets linked
  in. Dead-stripped from the final binary in `--release` builds.

There is **one** runtime — not two. The compiler picks which
symbols to reference. This keeps the runtime maintainable and
guarantees the debug + release paths share underlying state
(e.g., the live-set struct layout) where it matters.

---

## Phasing

Implementation order, smallest first:

1. **Source-tagged drop-chain entries** (this doc § above). The
   foundation that all other diagnostics build on.
2. **SIGABRT handler** that walks the chain and prints head
   entry's source tags. Immediately useful — turns every glibc
   abort into something diagnosable.
3. **`--live-set=strict`** with assert-on-duplicate behavior.
   Catches a class of compiler-codegen bugs in the live-set hook.
4. **`--poison-free`**. Simple memset-on-free.
5. **`--drop-chain-validate`**. Cheap and high-value.
6. **`--diag-hints`** (compile-time, not runtime). Quality of
   life for the diagnostic verbosity work.
7. **Stack-trace capture on throw**. Requires symbol-map
   plumbing.
8. **`--use-after-move-rt`**. Niche; static analysis catches
   most.
9. **`--profile-counters`**. Foundation for PGO. Independent of
   the rest.

The flavor-flag plumbing in `Compilation.md` exists already;
each feature here gets a CLI flag wired in as it lands. Flavor
defaults aren't binding until enough features land to give the
flavor meaningful semantics — until then, individual flags are
the supported surface.

---

## Open questions

- **Per-feature flag scoping.** Are toggles per-module (you can
  build one source file with `--source-tags=on` and link with
  release modules) or whole-program? Lean: whole-program — module
  granularity would mean the runtime needs both struct shapes
  coexisting, which is the complexity we're explicitly avoiding.
- **`cajeta.json` profile syntax.** The manifest is JSONC (not
  TOML); a `settings.profiles` block mirroring Cargo's
  `[profile.dev]` / `[profile.release]` shape is the lean. Resolved
  by the build tool, which passes the chosen `--mode` to the compiler.
- **Trap vs throw on `--bounds=on`.** Today the bounds check
  raises an exception. Should debug mode trap (faster, shows in
  a debugger as a clean SIGILL with a source map entry) while
  release throws (so user code can catch)? Lean: keep both as
  throws; introduce `--bounds=trap` as the explicit "I want a
  trap" knob. Document under the feature.
- **Cross-mode archive linking.** A library compiled in
  `--release` is consumed by a `--debug` app build. Today the
  archive includes pre-compiled IR; the consumer's mode wins on
  consumer-side codegen, but the library's `__cajeta_drop_push`
  calls were emitted in release shape and can't grow to debug.
  Lean: document that source tags + poison + similar features
  cover only code compiled in the same invocation; library code
  remains in its own mode and crosses the boundary "blind." A
  later mechanism could re-compile library bitcode in the
  consumer's mode at link time, but that's a v2 concern.
- **Should `--minimal` exist?** "No safety nets, smallest
  possible binary" is a real audience (embedded, microservices,
  WASM) but the surface is wide. Lean: keep it, with the
  defaults shown in § Mode profiles. Document it as "you have
  guaranteed correctness via reasoning."
