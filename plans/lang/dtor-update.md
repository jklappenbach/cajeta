# Plan: `~Object` — a virtual destructor at the root of the class hierarchy

Status: **DONE (2026-06-01, commit `f089308`).** `~Object()` — a virtual destructor at
the root of the hierarchy — landed, with drop/destructor breakpoints and
`debug-tests/dap/DropBreakpointTests.cpp`. (Shipped as one test file rather than the four
originally listed; feature coverage is equivalent.)

## 1. Goal

Give `cajeta.lang.Object` a destructor — `~Object()` — so that **every class has a
virtual, inheritable, overridable destructor rooted at `Object`**. This completes the
OO model ("destructors are virtual — that's the whole idea") and gives developers a
single, uniform answer to *"when was this instance destructed?"*: set an ordinary
breakpoint on the relevant `~T()` body.

This **replaces** the originally-scoped CP7-6 "drop/destructor breakpoint" machinery
(synthesized-wrapper synthetic safepoint + a bespoke "break on drop of T" breakpoint
type). We decided we **don't need that** — the developer just sets a breakpoint on the
dtor, which already works because destructor bodies carry per-statement safepoints
under `--debug-info`.

## 2. Why this works with the existing machinery (no debugger-specific codegen)

Investigated and confirmed:

- **Syntax → method.** `~ClassName() { ... }` is parsed by
  `CajetaLlvmVisitor::visitDestructorDeclaration` into a method internally named
  `drop` (void, no params). The name must match the enclosing class.
- **Virtual dispatch already exists.** The vtable carries a dedicated drop-fn slot at
  `vtable[0]` (`CAJETA_VTABLE_DROP_FN_OFFSET`); `__cajeta_class_virtual_drop(instance)`
  loads the instance vtable and calls that slot → the compiler-synthesized per-class
  drop wrapper (`__cajeta_<Class>_drop`, `getOrCreateDropFunction`).
- **The wrapper runs the chain.** The wrapper calls the class's own `drop` body
  (`emitDropBodyInline` → `b.CreateCall(userDrop)`), then each ancestor's drop body in
  reverse-DFS deduped order (`collectDestructorChain`), then `__cajeta_free` once.
- **`drop` is a normal virtual method.** The vtable builder skips constructors / static
  / abstract / templates — but **not** `drop`. So a `drop` method gets a regular virtual
  slot, and overrides are matched by **suffix** (`drop()`), hash-keyed (not positional).
  → A subclass `~T()` therefore *overrides* `~Object()` through the ordinary
  override mechanism. This is the virtual-destructor semantics, already supported.
- **Safepoints already in dtor bodies.** A `drop` body is a normal method body compiled
  through `Block`, which emits `__cajeta_dbg_safepoint(locId)` per statement under
  `--debug-info`. So a breakpoint on a `~T()` line already parks via the existing
  DebugController rendezvous — no new stop path, no protocol extension (FR-9.2/9.5).

**Conclusion:** the only change needed is *declaring* `~Object()`. Everything else is
existing behavior.

## 3. The change

### 3.1 Code (one edit)

`runtime/src/cajeta/lang/Object.cajeta` — add an empty destructor:

```cajeta
    /**
     * Virtual destructor — the root of every class's drop. (doc: see §)
     */
    ~Object() {
    }
```

Empty body: `Object` owns no resources; the synthesized wrapper still performs field
auto-drops and frees the instance. The value is the *root declaration* that subclass
`~T()` overrides and that anchors the "every class is destructible" model.

### 3.2 Docs (no MemoryModel.md edit from this thread)

- `docs/Debugging.md` — in the existing drop-breakpoint section, state that a
  drop breakpoint is an **ordinary source breakpoint on `~T()`** (no special type), and
  that overriding `~T()` is how you make a class's destruction observable.
- `ide-plugins/idea/ide-plugin-debug-fr-1.md` — mark CP7-6 done with this approach;
  record that the synthesized-wrapper safepoint and bespoke breakpoint type were
  **dropped by decision** in favor of `~Object` + breakpoint-on-dtor.
- **Hand-off note (do NOT edit here):** `docs/stdlib/MemoryModel.md §
  Destructors` and `stdlib/Lang.md § Object` should document `~Object` — that surface
  is owned by the **memory-management workstream**; flag it for them rather than editing
  from this (debugger) thread.

### 3.3 Explicitly NOT doing

- No synthetic safepoint in `getOrCreateDropFunction`.
- No new `XBreakpointType` / "break on drop of T" affordance in the plugin.
- No change to drop/ownership *semantics* (what drops, when, or the chain order).

## 4. Blast-radius analysis (why this needs the FULL suite, not just the 112)

`Object` is the implicit base of **every** class, so the change is not local:

1. **Every class's vtable gains a `drop` virtual slot** inherited from `Object`.
   Dispatch is hash-keyed, so this *should* stay correct, but indices shift across all
   vtables — must verify nothing positional breaks.
2. **Classes that already declare `~T()` (Buffer, Stream, File, …)** will now have their
   `drop` detected as an **override of `Object.drop`** (same `drop()` suffix) instead of
   a fresh slot. Vtable construction for those classes changes — verify they still build
   and run.
3. **Every class's synthesized drop wrapper gains a call to `Object.drop()`** (empty) as
   the final ancestor in the chain. Functionally a no-op call; verify it resolves
   cross-module (`ensureFunctionInModule`) and adds no measurable issue.
4. **`__cajeta_drop_count`** is bumped only in `__cajeta_drop_pop_run`, not by
   `emitDropBodyInline`'s direct `CreateCall`. So the empty `Object.drop()` call should
   **not** change drop counts — but several tests assert drop counts, so confirm.
5. **`Object` itself** now gets a synthesized drop wrapper + vtable drop-fn patch. Verify
   no recursion (Object.drop is empty; Object has no parent → empty chain → one free).

## 5. Risks & mitigations

| # | Risk | Mitigation |
|---|------|------------|
| R1 | Vtable index shift breaks virtual dispatch somewhere positional | Full-suite before/after diff; targeted polymorphism tests |
| R2 | `~Buffer()`/`~Stream()`/`~File()` now override `Object.drop` and mis-build | Build stdlib; run any Buffer/Stream/File tests + a focused dtor test |
| R3 | Cross-module call to `Object.drop` from a user module's wrapper fails to link | JIT debug-session test that constructs+drops a user class |
| R4 | Drop-count assertions shift | Grep tests for `drop_count`; run them; confirm unchanged |
| R5 | Double-free / recursion via Object in the chain | Inspect emitted IR for one class; assert single `__cajeta_free`; run ASAN-style drop tests if present |
| R6 | Memory-workstream collision (their in-flight drop changes) | Coordinate; keep change minimal + reversible; don't touch MemoryModel.md |
| R7 | Flaky full suite hides a real regression | **Baseline diff**: capture failing set BEFORE, compare AFTER; only *new* failures count |

## 6. Test plan

### 6.1 Pre-change baseline (required, because the full suite is flaky)
- Record current commit (`62f7b7f`-era / `f569c14`) green state:
  - C++ **debug** suite: expect **112/112** (reliable).
  - Plugin suite: **87/87** (reliable).
  - **Full** `cajeta_test` suite: capture the set of failing test names to a baseline
    file (serial run per the Windows hazard; `taskkill cajeta_test.exe` first). This is
    the known ~170-failure + flaky baseline to diff against.

### 6.2 After-change verification
- **Build**: compiler + stdlib bitcode rebuild clean (Object.cajeta change forces a
  stdlib recompile).
- **C++ debug suite**: still **112/112**.
- **Plugin suite**: still **87/87** (no plugin change expected; sanity only).
- **Full `cajeta_test` suite**: serial run; **diff against the 6.1 baseline** — pass
  criterion is *no new failures* (pre-existing flaky failures excluded).

### 6.3 New tests (the actual feature)
Added to the C++ debug suite (`debug-tests/...`, JIT DapServerSession-style):

1. **`DropBreakpointStopsOnDestructor`** — program with a class `Foo` that declares
   `~Foo()` with a body statement; create a `Foo`, let it drop at scope exit; set a
   breakpoint on the `~Foo()` body line; assert the session **stops there** at drop
   (FR-9.1 primary UX).
2. **`DroppedInstanceInspectableAtDtorStop`** — at that stop, assert the dropped
   instance's `this`/fields are inspectable via `variables` (FR-9.3). *(May reveal that
   the dtor frame doesn't register `this` as a debug local — if so, that becomes a
   small follow-up; the plan notes it rather than silently passing.)*
3. **`VirtualDestructorOverrideDispatches`** — base `Animal` with `~Animal()`, derived
   `Dog` with `~Dog()`; drop a `Dog` through an `Animal` reference; assert the
   breakpoint on `~Dog()` (not `~Animal()`) is the one that fires — proving virtual
   dispatch of the override. (Or assert via a side-effect/`drop_count` if a breakpoint
   race is fiddly.)
4. **`ClassWithoutExplicitDtorStillDropsCleanly`** — a class with no `~T()` still
   constructs/drops correctly after the change (regression guard for the empty
   `Object.drop()` chain call).

### 6.4 Decision gate
If §6.2 full-suite diff shows **new** failures attributable to the change that aren't
quickly fixable, **stop and report** rather than chase a cascade — fall back to "no
`~Object`, breakpoint-on-`~T()` already works for declared dtors" (the change is one
file + reversible).

## 7. Execution order
1. Capture baselines (§6.1) — debug suite, plugin suite, full-suite failing set.
2. Apply the one-line `~Object()` edit.
3. Rebuild compiler + stdlib; confirm clean build.
4. Run C++ debug suite (112) + write the 4 heap tests (TDD: write first, watch them fail
   for the right reason, then they pass with the change in place).
5. Run full suite serially; diff against baseline.
6. Update docs (Debugging.md, FR-1 table); leave MemoryModel.md to the memory thread.
7. Commit per checkpoint; update memory.

## 8. Rollback
Single-file change. `git checkout -- runtime/src/cajeta/lang/Object.cajeta` + rebuild
reverts completely. No data migration, no protocol change.

## 9. Open questions for review
- **Q1.** OK that this **supersedes** the FR-9 "break on drop of T (no dtor)" capability?
  After this, a class with no `~T()` is not individually drop-breakable (you'd add a
  `~T()`). Confirmed acceptable per "developer only needs to set a breakpoint on the
  dtor"?
- **Q2.** Acceptable that I edit `Debugging.md` + the FR doc but **not** `MemoryModel.md`
  (hand the stdlib-doc update to the memory workstream)? Or do you want me to update
  MemoryModel.md too?
- **Q3.** For test #2 (inspect dropped instance at the stop): if the dtor frame doesn't
  expose `this` as a debug local today, do you want that wired in now (small extra
  scope) or logged as a follow-up?
- **Q4.** Full-suite baseline-diff is the only honest verification given the flaky
  suite. Good with that approach (vs. trying to get the full suite fully green first,
  which is out of scope)?
