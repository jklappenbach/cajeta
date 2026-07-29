# Thread-Safe Compiler — Specification

> Status: APPROVED (2026-06-28).
> Decision record: sharing model = **frozen-shared stdlib + per-thread state**;
> sequencing = **spec/plan now, land before v0.8.0**.
> §8 resolved: 8.1 = (b) freeze lazy pkgs under a one-time lock then share;
> 8.2 = `thread_local` current-context pointer; 8.3 = freeze-after-prime +
> lock-free read; 8.4 = process one-time inits stay global (idempotent).

## 1. Definition

### 1.1 Purpose
Make the Cajeta compiler **re-entrant** so that multiple, independent compilations
can run **concurrently on threads within a single process**, sharing one
immutable stdlib image. Today every piece of cross-compile state lives in
process-global mutable variables (~49 distinct items), so two compiles in one
address space corrupt each other; isolation is achieved only by running each
compile in its own OS process.

### 1.2 Problem
1. **No re-entrancy.** ~40 free-standing statics (`canonicalMap`, `methods`,
   `Method::archive`, the `g_archive` family, `activeModule`/`currentCodegenModule`,
   `typeMap`/`llvmTypeIdMap`, lazy-stdlib bookkeeping, …) are read and mutated
   throughout parse and codegen. Concurrent compiles race on all of them.
2. **stdlib re-primed per isolate.** Because state is global, each isolated
   compile re-parses + re-codegens the whole stdlib (~14 s, ~360 MB/process). The
   test suite's in-process reuse cache (`StdlibReuseCache`, capture/restore
   baseline) tried to amortize this but reintroduced cross-test contamination
   (the multi-suite hang, the eviction/reparent units) — proof that *sharing
   mutable global state between two compiles is the wrong primitive*.
3. **Memory.** N isolated compiles cost N × the stdlib image. Sharing one
   read-only stdlib across N thread-workers is the only way to beat
   process-per-test on memory (a `thread_local`-everything design would make it
   *worse* — N private copies in one address space).

### 1.3 Approach (decided)
Split all cross-compile state into two tiers:

- **Frozen semantic model** — the stdlib's types, methods, signatures, parsed
  declarations: built **once** at prime, then made **immutable and
  context-free**, and shared **read-only** by every thread.
- **Per-thread compile state** — everything mutable for one compilation: the
  user types/methods/archives, the "what am I compiling now" pointers, name
  counters, and the **LLVMContext** + codegen bindings. Owned by a
  `CompilationContext`, one per concurrent compile, reached through a
  `thread_local` current-context pointer.

LLVMContext is not thread-safe, so codegen bindings (`llvm::Type*`,
`llvm::Function*`, `GlobalVariable*`) cannot live on the shared frozen model;
they move to a **per-thread binding side-table** keyed by the frozen
`CajetaType*`/`Method*`. The JIT shares the stdlib at the **ORC layer**: the
frozen stdlib is materialized once into a shared `JITDylib`, and each compile's
user module links against it (ORC supports concurrent compilation).

### 1.4 Non-goals
- **Intra-compile parallelism.** A *single* compilation stays single-threaded.
  We isolate *between* independent compiles; we do not parallelize one compile.
- **Production AOT behavior change.** `--emit=exe/obj` single-compile output must
  be byte-for-byte unchanged; thread-safety must not alter codegen results.
- **Removing process mode.** Running compiles in separate processes must keep
  working; threads become an *option*, not a requirement.

### 1.5 Glossary
- **Frozen model** — immutable, context-free stdlib semantic objects, shared.
- **CompilationContext (ctx)** — owns one compile's mutable state + LLVMContext.
- **Current-context pointer** — `thread_local CompilationContext*` set at compile
  entry; accessors resolve global-looking calls to the calling thread's ctx.
- **Binding side-table** — per-thread map from frozen `CajetaType*`/`Method*` to
  that thread's LLVM `Type*`/`Function*`/`Global*`.

---

## 2. Per-thread compile state (`CompilationContext`)

All mutable cross-compile state currently in statics is owned by a
`CompilationContext` and reached through a `thread_local` current-context
pointer, so existing call sites change accessor *backing* (not call shape) and
each thread resolves to its own state.

### Use cases
- **2.1** As the compiler, when a `CompilationContext` is constructed and made
  current, then a compile sees pristine per-thread registries (the de-facto
  `resetGlobals()` set: `canonicalMap`, `typeMap`, `llvmTypeIdMap`, the
  `g_archive` family, `methods`, `Method::archive`, `aspectClasses`,
  `componentClasses`, `factoryClasses`, `activeProfile`, `activeModule`,
  `currentCodegenModule`, …) without touching any other thread's state.
- **2.2** As two threads compiling different sources at the same time, when both
  resolve a type by name, then each reads/writes only its own
  `canonicalMap`/`typeMap` — no shared mutation, no lock on the hot path.
- **2.3** As the compiler, when a compile finishes (ctx destroyed), then all its
  per-thread state and its LLVMContext are released deterministically — no
  capture/restore baseline, no cross-compile residue.
- **2.4** As name synthesis (lambda/method-ref/spawn-trampoline counters), when
  run under a ctx, then counters are **per-ctx** and reset per compile, so
  synthesized names are deterministic and never collide across threads.
- **2.5** As a deeply-nested call site (no ctx parameter available), when it
  accesses former-global state, then it resolves through the `thread_local`
  current-context pointer and reaches the correct thread's ctx.

---

## 3. Frozen-shared stdlib semantic model

The stdlib is primed once into semantic objects that are then **immutable** and
**context-free**, shared read-only across all threads.

### Use cases
- **3.1** As the runtime, when the stdlib is primed once, then its types/methods
  are frozen and every subsequent compile *reads* them without copying the
  semantic model.
- **3.2** As a compile, when it resolves a stdlib type (e.g. `String`,
  `ArrayList`), then it gets a pointer into the shared frozen model — not a
  per-thread clone of the semantic object.
- **3.3** As a compile that instantiates a stdlib template over a **user type**
  (`ArrayList<test.Foo>`), when monomorphization runs, then the new
  instantiation's semantic objects + IR are created in the **per-thread** ctx /
  user module — the frozen stdlib is never mutated.
- **3.4** As the system, when any code path attempts to mutate a frozen stdlib
  object (add a method, set an llvm binding on it, register an instantiation on
  it), then it **fails loudly** (assert/throw in debug) so the leak is caught at
  its source rather than corrupting another thread.
- **3.5** As the lazy-stdlib loader (`cajeta.math`, …), when a package is parsed
  on demand, then it either (a) is primed + frozen up front, or (b) is loaded
  into per-thread state — but never lazily mutated into a shared frozen image
  mid-run. (OPEN — see §8.1.)

---

## 4. Codegen-binding separation (per-thread LLVMContext)

Codegen-time LLVM pointers move off the shared semantic model into a per-thread
binding side-table, so each thread builds IR in its own LLVMContext.

### Use cases
- **4.1** As a compile, when it needs the `llvm::Type*` for a frozen stdlib
  `CajetaType`, then it looks it up in *its* binding side-table (creating it in
  *its* LLVMContext on first use), never reading an llvm pointer off the frozen
  object.
- **4.2** As two threads, when both materialize the LLVM type for the same frozen
  `String`, then each gets a type in its own context — no shared
  `typeMap`/`llvmTypeIdMap`, no cross-context pointer.
- **4.3** As a compile, when codegen finishes and the ctx is destroyed, then its
  binding side-table + LLVMContext are freed with no dangling pointers left on
  the frozen model (because none were ever stored there).
- **4.4** As the existing reuse machinery, when this lands, then
  `s_sharedContext`, `captureBaseline`/`restoreBaseline`, `reuseEpoch`, and the
  reuse-hazard gate are **removed** — superseded by structural isolation.

---

## 5. JIT/link stdlib sharing (ORC)

The frozen stdlib is materialized once into a shared ORC `JITDylib`; per-compile
modules link against it, sharing the JIT'd stdlib code across threads.

### Use cases
- **5.1** As the JIT host, when the process starts, then the frozen stdlib is
  emitted/materialized **once** into a shared stdlib `JITDylib`.
- **5.2** As a compile, when its user module is added, then it is placed in a
  per-compile `JITDylib` that *links against* the shared stdlib dylib — no
  per-compile clone-and-link of the whole stdlib IR.
- **5.3** As N concurrent compiles, when each looks up its entry symbol, then ORC
  resolves user symbols per-dylib and stdlib symbols from the one shared dylib,
  concurrently and without corruption.
- **5.4** As the memory budget, when N compiles run, then total RSS ≈ one shared
  stdlib image + N × (small per-compile user state) — not N × full stdlib.
  (Acceptance target measured in §7.)

---

## 6. Concurrent execution & the test harness

The harness gains a single-process, multi-thread mode that primes once and runs
tests across a thread pool sharing the frozen stdlib — and the serial
multi-suite path is removed.

### Use cases
- **6.1** As the test harness, when run in threaded mode, then it primes the
  stdlib once and dispatches tests/suites to a worker-thread pool, each worker
  compiling+JITing in its own ctx against the shared frozen stdlib.
- **6.2** As the developer running `./cajeta_tests.sh A B`, when multiple suites
  are selected, then they run isolated (each on its own ctx) with **no special
  serial-multi-suite code path** and **no contamination** — the path that exists
  today only because of shared global state is gone.
- **6.3** As the full sweep, when run threaded, then peak RSS and wall-clock both
  improve versus the current per-suite-process + reuse model (targets in §7).
- **6.4** As production `--emit`, when invoked, then it runs a single ctx exactly
  as before — no behavior or output change (§1.4).

---

## 7. Correctness, enforcement & parity

### Use cases
- **7.1** As the test suite, when run threaded vs single-threaded, then the
  pass/fail set is **identical** (a parity gate guards the whole effort).
- **7.2** As a stress test, when many ctxs compile concurrently (incl. the same
  source on multiple threads, and template-instantiation-heavy sources), then
  results are deterministic and ThreadSanitizer reports **no data races** on
  compiler state.
- **7.3** As a freeze-violation guard, when any write to the frozen model is
  attempted, then it aborts in debug builds (§3.4) — and the test suite includes
  a case proving a user-type stdlib-template instantiation does **not** touch the
  frozen model.
- **7.4** As the memory acceptance, when the threaded sweep runs at N workers,
  then measured peak RSS ≤ (single stdlib image + N × per-compile state) and is
  **lower** than the current per-suite-reuse peak (~0.9 GB/proc baseline).
- **7.5** As production AOT, when a corpus of `--emit=exe` builds is compiled
  before and after, then emitted objects are unchanged (byte-diff clean).

---

## 8. Open questions (resolve before/within the plan)

- **8.1 Lazy stdlib under freeze.** `cajeta.math` & friends are parsed on demand
  today. Options: (a) prime + freeze *all* stdlib (incl. lazy pkgs) up front —
  simplest to reason about, costs prime time/memory for unused pkgs; (b) freeze
  lazily with a one-time global lock the first time a pkg is needed, then shared;
  (c) load lazy pkgs into per-thread state (no sharing for those). Recommend (b):
  shared benefit, pay only for what's used. **Needs decision.**
- **8.2 Access mechanism.** `thread_local` current-context pointer (recommended:
  minimal call-site churn, true per-thread isolation) vs threading a `ctx&`
  parameter through every API (cleaner dependency graph, very large churn).
  Recommend `thread_local` pointer. **Needs confirmation.**
- **8.3 Interned global caches.** `QualifiedName::cache` and the stdlib
  package-index are read-mostly and currently unbounded-shared. Keep shared
  behind a lock / make immutable after prime, or move per-thread? Recommend
  freeze-after-prime + lock-free read. **Needs decision.**
- **8.4 Process-level one-time inits** (LLVM target init, NVPTX/AMDGPU
  `once_flag`, optimizer tuning) stay process-global (already idempotent) — no
  change. Confirm.
