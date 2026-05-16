---
title: 'Async Runtime — Implementation Status'
layout: '~/layouts/MarkdownLayout.astro'
category: 'Status'
description: 'Tracks the R1–R5 rollout of the async runtime described in ThreadModel.md. Counterpart to ImplementationStatus.md (which covers the MemoryModel rollout).'
---

Tracks the R1–R5 rollout of the async runtime described in `ThreadModel.md`. Counterpart to `ImplementationStatus.md` (which covers the MemoryModel rollout).

---

## Current status

**Phases R1 through R5-D + error-model v1 complete.** Full structured-concurrency story functional end-to-end: stackful fibers, scope joins, cancellation, exception escalation. Error model has stdlib Throwable hierarchy (in `package cajeta.error;`, with `cause` chaining), `throws` clause grammar + advisory lint with try/catch coverage awareness, runtime exception path on `void*`, Task<T> exception slot with await re-raise, and stack-trace capture at throw sites with auto-print on uncaught.

**Post-v1 polish items — all shipped:**
- [x] Recoverable/Unrecoverable distinction via vtable type-check (#210) — commit `ea5ca6e`.
- [x] Inherited-field write codegen fix (#208) — commit `16434e5`, with `0dda81b` follow-up for the inherited-field ASSIGN slot width + the orthogonal `(T) obj.field` cast-load bug surfaced by stress testing.
- [x] `String` parameter codegen fix (#211) — commit `b987e59`. `Throwable.message` is now `String`.
- [x] Lint refinements (#209) — commit `996cf21`. The uncaught-throws lint now walks an enclosing-try stack and suppresses warnings when any catch arm covers the throw (supertype-aware). The annotation generalized to `@SuppressLint("id")` in `e3d238e`.

**Still open:**
- A few smaller known gaps and v2 concerns — see *Known gaps* below.

---

## Plan: Task<T> as user-typeable template  ✅ complete

Today the compiler synthesizes `Task<T>` at every spawn site (`CajetaTask::getOrCreate(module, T)` called from `SpawnExpression::resolveTypes`); users can now also write `Task<int32>` as a variable type. The canonical workload of storing two task handles before awaiting either is now expressible.

### Punch list

- [x] **20.1** Type resolution: `Task<int32>` as a `typeType` parses through `classOrInterfaceType` already (the grammar accepts type arguments uniformly). The miss was in `CajetaType::fromContext(TypeTypeContext*)`: when the bare name `Task` is followed by `typeArguments`, route through `CajetaTask::getOrCreate(module, T)` instead of looking `Task` up as a user class. Special-case branch added ahead of the generic template path so we don't need a fake "Task" template class to satisfy `isTemplate()`.
- [x] **20.2** Ownership transfer (option a — auto-promotion on assignment). Added a `llvm::Value* dropEntry` field on `SpawnExpression` populated when the push happens. `LocalVariableDeclaration`'s initializer path detects a `SpawnExpression` RHS via `dynamic_pointer_cast` and emits `__cajeta_drop_mark_inactive` on the entry; the local's own class-instance drop becomes the canonical owner. Bare-statement `spawn foo();` is unaffected — no assignment, no inactivation, the spawn's drop still fires at scope exit. (BinaryOpExpression ASSIGN didn't need the same: today's tests rebind via re-declaration, not via plain `=`; will revisit when that shape lands.)
- [x] **20.3** `await someTaskLocal`. Two pieces had to land: a defensive `inner->resolveTypes(module)` re-resolve at codegen time in `AwaitExpression::generateCode` (the pre-pass runs before the local is in scope and leaves resolvedType null, which sent the dispatch into the sync-compat branch returning the raw Task ptr), AND a `SpawnExpression` preempt in `loadIfLValue` so `Task<int32> t = spawn foo();` doesn't try to load the 24-byte Task value through an 8-byte ptr slot — for class types the value IS the pointer.
- [x] **20.4** Package question. Decided: keep `CajetaTask` package-free (the comment at `CajetaTask.cpp:17` already says so). Users write the bare name `Task<int32>`; type resolution finds it directly through the synthesized-type path. Revisit when `cajeta.threading.*` actually lands.
- [x] **20.5** Tests in `test/parser/TaskTypingTests.cpp` (new file): `declareAwaitOneTask`; `twoHandlesStoredThenAwaited` (the canonical use case); `differentTaskElementTypes` (Task<int32> vs Task<int64> get distinct value-field widths); `declaredTasksDropOncePerLocal` (drop count == 2 for two declared tasks, proving option-a's inactivation); `awaitedTaskStillDropsOnceAtScopeExit`.

### Race fix surfaced en route (commit `ca2b0d7`)

While probing item 20.2, `SpawnDropTests` flaked ~20% as a suite. Probe isolated `free(task)` in `__cajeta_task_drop` as the trigger. Root cause was unrelated to the typing work itself: `ScopeStatement::generateCode` emitted `scope_enter → body → DROPS FIRE → scope_exit` for explicit `scope { }` blocks. But `__cajeta_scope_exit` walks every registered task's `exception_addr` (a pointer INTO the task allocation) — so the drop's `free(task)` ran before the scope_exit read through it. Use-after-free that the allocator usually masked. Fixed by reordering `ScopeStatement` to manage its block's drop frame manually and emit drops AFTER `__cajeta_scope_exit`. Method body's fall-through path already had this invariant (scope_exit_to before emitOwnerDrops); only the explicit-scope path was wrong.

### TLS-promote drop-chain head + exception chain head  ✅ complete

`MemoryModel.md` § Known gaps and `AsyncStatus.md` both flagged the drop and exception chain heads as single static globals, "correct under today's single-carrier model but a multi-carrier pool would race." That framing under-stated the actual risk: the carrier thread is already a separate OS thread from main, so even single-carrier work races on the globals when main and the carrier are both active.

Promotion landed on the same pattern `scope_top` already uses — per-fiber slots for code running inside a fiber, a `__thread` TLS slot for the main thread:

- `cajeta_fiber` gained `drop_top` and `exc_top` fields, zero-initialized in `__cajeta_task_run`.
- `__cajeta_main_drop_top` / `__cajeta_main_exc_top` are `__thread`-qualified per-OS-thread slots.
- `__cajeta_drop_top_ptr()` / `__cajeta_exc_top_ptr()` helpers return the right slot based on `__cajeta_current_fiber`, mirroring `__cajeta_scope_top_ptr()`.
- `__cajeta_drop_count` stays a process-global (it's test observability across both threads), but increments and the get/reset accessors now go through `__atomic_*_n(..., __ATOMIC_SEQ_CST)` so drops fired on the carrier are visible to main.
- All call sites (`drop_push`, `drop_pop_run`, `exc_push`, `exc_pop`, `throw`, `get_thrown`) read/write through the helpers.

New test `SpawnDropTests.carrierDropsAccountedSeparately`: a spawned method declares its own `int32[]` local. The spawned-method's drop fires on the carrier's per-fiber chain; main's two `Task<int32>` drops fire on main's TLS chain. Atomic counter sums them. Without TLS promotion, the carrier's push/pop would corrupt main's chain (shared global head).

### Real `detach` (fire-and-forget) + `#`-captures rule  ✅ complete

`DetachExpression::generateCode` was previously a sync passthrough — it just `inner->generateCode(module)`'d the call, so `detach foo();` ran inline on the caller's thread, indistinguishable from a bare call. The fire-and-forget semantics `ThreadModel.md` describes weren't actually implemented.

Spec tightened first (`ThreadModel.md` § detach Semantics (v1)): the inner must be a method call; captures must each be `#`-transferred, primitive, or a fresh `new T(...)`; the Task wrapper leaks for the process lifetime; exceptions in detached bodies are captured to the Task's exception slot but never observed (no awaiter, no scope). The leak is the explicit "use sparingly" trade-off.

Implementation reuses `SpawnExpression`'s full trampoline + fiber-enqueue lowering — a new `detachMode` flag on `SpawnExpression` gates the two pieces detach skips:

- `__cajeta_scope_register` is skipped — `scope_exit` won't wait on this Task.
- The Task's drop-chain push is skipped — no scope owns the Task; it leaks.

`DetachExpression::generateCode` resolves the inner's argument types, runs `enforceDetachMoveOnlyCaptures` (the `#`/primitive/`new` check, throwing `CAJETA_ERROR_DETACH_BORROW_CAPTURE` on violation), then constructs a transient `SpawnExpression` wrapping the same `MethodCallExpression` with `detachMode=true` and delegates. Single source of truth for the trampoline.

Auxiliary fix: `SpawnExpression`'s value-store path now skips when the inner returns void (`isVoidTy()` check) and `CajetaTask::CajetaTask` substitutes an `i8` placeholder for the value slot when `elementType` is void — LLVM forbids storing void or putting it in an aggregate field. Previously latent because all spawn tests used int-returning inner functions; surfaced when `detach foo();` runs an `async void foo()`.

New tests in `test/parser/DetachTests.cpp`: no-capture detach returns immediately; primitive captures work; `#`-transferred class captures work; bare class-typed identifier (borrow capture) is rejected at compile time. Fresh-allocator (`new T()` inline as arg) is documented as a Known gap rather than tested directly — the underlying NewExpression-as-method-call-argument codegen path is broken even for non-detach calls (`consume(new Payload())` crashes), so the detach-specific check accepting `NewExpression` will light up the moment that gap is fixed.

### Out of scope for this rollout

- Task<T> as a field type on a user class.
- Task<T> as a method return type (would require the async lowering to materialize a Task at the return site).
- Task<T> in a generic position of another template (`Box<Task<int32>>`). Should fall out of monomorphization, but isn't tested.
- Move-out via `#t` (transferring a Task local to a method). The drop chain machinery supports it, but it isn't a Task-specific concern — covered by the general move-out rules when those tests are added.

### Ownership-transfer model — why option (a)

`MemoryModel.md` § Borrow / transfer rules: *"Auto-promotion for fresh `new`. An anonymous `new T(...)` expression in transfer position promotes implicitly. ... The temporary is an unnamed owner with no prior identity, so promotion has no use-after-move risk."* Spawn is a fresh allocator with no prior identity — same rule applies. The drop-chain primitive `__cajeta_drop_mark_inactive` already exists for `#`-move; we're invoking it from a new call site, not inventing a mechanism.

Two alternatives considered and rejected:
- *(b) Local skips its own drop when the initializer is a Spawn.* Localized but breaks `#`-move-out of Task locals — with no entry to deactivate, ownership can't be transferred out of the local later. Makes Task a second-class citizen for the move syntax.
- *(c) Spawn never pushes a drop; bare-statement spawn synthesizes an anonymous local.* Cleanest "every owner has a name" story, but touches `ExpressionStatement` or the parser. Larger blast radius than the punch list above warrants.

---

## Completed

### Foundation (pre-R1)
- Grammar additions: `async`, `await`, `spawn`, `scope`, `detach` keywords.
- AST nodes: `AwaitExpression`, `SpawnExpression`, `DetachExpression`, `ScopeStatement`.
- Async modifier carried on `Method` (`ASYNC` bit in `Modifiable`).
- Sync-lowering MVP — all five keywords parse and codegen as pass-throughs; the surface syntax is stable. Commit `2a5a11b`.

### R1 — Task<T> wrapper
- `CajetaTask` class synthesized per element type (like `CajetaArray`).
- Layout: `{ T value, i32 done }`; cached on the module's structure map.
- spawn allocates Task<T> on heap, packs result + done flag.
- await unwraps via field GEP.
- Commit `a98b6e7`.

### R2 — Pthread-backed task queue + worker scheduler
- Codegen-emitted per-spawn-site trampoline; the worker invokes it on a shared queue.
- `__cajeta_task_run`, `__cajeta_task_wait`, `__cajeta_task_complete` runtime helpers.
- Zero-arg method calls only (arg capture deferred to R3-A).
- Commit `0cd0734`.

### R3-A — Arg capture for spawn
- Heap-allocated context struct `{ ptr task, arg0, arg1, ... }` per spawn site.
- Args evaluated at the spawn site so side effects stay on the calling thread.
- Trampoline loads args from ctx and routes through `targetClass->invokeMethod`.
- Commit `90eb566`.

### R3-B v1 — Stackful fiber executor
- Decision: stackful (Java 21 virtual thread model) over stackless state machines. Removes function coloring; no async-fn codegen transformation.
- `ucontext.h`-based fiber primitive with ~64 KB per-fiber stack.
- Single carrier OS thread runs many fibers cooperatively.
- `await` from inside a fiber parks (swap to carrier); main-thread `await` OS-blocks on a condvar.
- `__cajeta_task_complete` wakes all parked fibers (polling-wake; per-task wait queues are a future optimization).
- ThreadModel.md updated to reflect the stackful decision and document the global drop-chain-head as a known gap.
- Commit `afc60fc`.

### R4 — Async-aware Lock
- Each lock wraps `{ pthread_mutex_t, pthread_cond_t, held flag, fiber wait queue }`.
- Fiber-path lock_acquire parks on the lock's own wait queue if held; release dequeues one waiter onto the ready queue.
- Main-thread lock_acquire uses per-lock condvar.
- Existing `__cajeta_lock_*` ABI unchanged (void*); all existing Lock/LockGuard tests still pass.
- Commit `a57e1a9`.

### R5-A — Explicit `scope { }` joins
- `scope_enter` pushes a per-fiber (or per-main-thread) scope frame.
- `SpawnExpression` registers each task's done-addr via `__cajeta_scope_register`.
- `scope_exit` walks the registered list and `__cajeta_task_wait`s on each before popping the frame.
- Commit `42e7ebe`.

### R5-A' — Implicit function-body scope (subsumes R5-B)
- Every method body is also an implicit scope. Method::generateCode captures `__cajeta_scope_save_top()` into an alloca at entry; every return path calls `__cajeta_scope_exit_to(watermark)`.
- Watermark-based exit pops all frames the method pushed (function-body frame + any explicit `scope { }` still open).
- R5-B (parse-time error for spawn-without-scope) is subsumed: every spawn is structurally inside *some* scope.
- Commit `a4bedb3`.

### Design spec
- `ErrorModel.md` — drafted. Errors-as-values, `try` mandatory at call sites, `panic` for unrecoverables, multi-arm pattern catches. Commit `ab68857`. Subsequently rewritten around an exception-hierarchy design (commit `6fe6cb6`); the rest of the bullets below describe the shipped version.

### Error model — implementation (v1 complete)
- Stdlib `Throwable` / `Exception` / `RecoverableException` / `UnrecoverableException` classes, originally in `package cajeta.lang;`, moved to `package cajeta.error;` for discoverability + reuse. `Exception` carries a typed `Throwable cause` for chain-of-causality.
- `throws` clause grammar + AST (#200), advisory `uncaught-throws` lint warning (#201), stack-trace capture at throw sites (#203).
- Runtime exception path migrated to `Throwable*` on `void*` carrier; system default catch wraps main + each fiber trampoline. `CajetaTask` gained an exception slot; `await` re-raises into the awaiter.
- Recoverable/Unrecoverable distinction at system catch (#210) — vtable type-check against the `__cajeta_unrecoverable_vtable_marker` global published by `Compiler::parse()` after stdlib load.
- R5-C cancellation tokens (commit `fa7c7f8`) — `CancellationException extends RecoverableException`; scope-level cancel walks registered children and sets their atomic cancel flag.
- R5-D exception escalation through scope — `scope_exit` joins children, cancels siblings on first throw, re-raises into the containing frame.
- Comprehensive lint system: rule IDs + `@SuppressLint("id")` (commits `ded5897`, `e3d238e`); try/catch coverage awareness for `uncaught-throws` (#209, commit `996cf21`) — the lint walks an enclosing-try stack and suppresses warnings when any catch arm catches the throw (supertype-aware via `getSuperClasses()`).
- Inherited-field write codegen fix (#208, commit `16434e5`) — subclass layout now prepends inherited fields, so GEPs for inherited fields share the parent's slot index. Follow-up (commit `0dda81b`) added the same walk to `BinaryOpExpression`'s ASSIGN slot-type lookup (mixed-width inherited fields were trampling neighbors with too-wide stores) and switched `CastExpression::generateCode` to `loadIfLValue` so `(T) obj.field` loads the value instead of ptrtoint'ing the GEP address.
- `String`-parameter codegen fix (#211, commit `b987e59`) — variable-size-field check now gates on `dynamic_pointer_cast<CajetaStruct>`, so a `String` field on a regular class is freely writable. Unblocked `Throwable.message: String`.
- `int8` / `uint8` wired as documented native types (commit `2a9655b`) — `INT8`/`UINT8` lexer rules and `CajetaType::init` registrations were missing, so `int8` lexed as IDENTIFIER and silently produced null types that segfaulted in class layout.
- Unknown field types throw `CAJETA_ERROR_UNKNOWN_TYPE` at `visitFieldDeclaration` instead of silently leaking null (commit `ac122a6`) — turns the same segfault shape into a diagnostic with the user's original type-name token.

---

## Known gaps surfaced by the rollout

_(None active. Earlier entries — TLS-promote, `detach` fire-and-forget, inline `new T(...)` as method-call argument — have all shipped.)_

---

## Notes / open questions

- **Worker thread pool.** Today's executor is a single carrier OS thread. A pool would let multiple fibers run truly in parallel, but raises new questions: the global drop-chain head needs TLS promotion (above); the wake-all-parked policy in `__cajeta_task_complete` would scale badly with many concurrent awaits; the lock and scope code already takes the right per-object locks but hasn't been audited under concurrent carriers. Probably a v2 concern.
- **Function-body implicit scope: cost.** Each method call now pays one malloc + one free for its scope frame, even if the method never spawns. Profiling will tell if this matters; the optimization is straightforward (lazy frame alloc on first scope_register) and can land when measured.
