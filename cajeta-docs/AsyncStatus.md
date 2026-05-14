# Async Runtime — Implementation Status

Tracks the R1–R5 rollout of the async runtime described in `ThreadModel.md`. Counterpart to `ImplementationStatus.md` (which covers the MemoryModel + WireFormats rollout).

---

## Current status

**Phases R1 through R5-D + error-model v1 complete.** Full structured-concurrency story functional end-to-end: stackful fibers, scope joins, cancellation, exception escalation. Error model has stdlib Throwable hierarchy (in `package cajeta.error;`, with `cause` chaining), `throws` clause grammar + advisory lint with try/catch coverage awareness, runtime exception path on `void*`, Task<T> exception slot with await re-raise, and stack-trace capture at throw sites with auto-print on uncaught.

**Post-v1 polish items — all shipped:**
- [x] Recoverable/Unrecoverable distinction via vtable type-check (#210) — commit `ea5ca6e`.
- [x] Inherited-field write codegen fix (#208) — commit `16434e5`, with `0dda81b` follow-up for the inherited-field ASSIGN slot width + the orthogonal `(T) obj.field` cast-load bug surfaced by stress testing.
- [x] `String` parameter codegen fix (#211) — commit `b987e59`. `Throwable.message` is now `String`.
- [x] Lint refinements (#209) — commit `996cf21`. The uncaught-throws lint now walks an enclosing-try stack and suppresses warnings when any catch arm covers the throw (supertype-aware). The annotation generalized to `@SuppressLint("id")` in `e3d238e`.

**Still open:**
- TLS-promote the exception chain + drop-chain head for multi-carrier safety. Today's single-carrier model doesn't race; a worker pool would. See *Known gaps* below.
- Several smaller known gaps and v2 concerns, also below.

---

## Completed

### Foundation (pre-R1)
- Grammar additions: `async`, `await`, `spawn`, `scope`, `detach` keywords.
- AST nodes: `AwaitExpression`, `SpawnExpression`, `DetachExpression`, `ScopeStatement`.
- Async modifier carried on `Method` (`ASYNC` bit in `Modifiable`).
- Sync-lowering MVP — all five keywords parse and codegen as pass-throughs; the surface syntax is stable. Commit `2a5a11b`.

### R1 — Task<T> wrapper struct
- `CajetaTask` class synthesized per element type (like `CajetaArray`).
- Layout: `{ T value, i32 done }`; cached on the module's structure map.
- spawn allocates Task<T> on heap, packs result + done flag.
- await unwraps via struct-GEP.
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
- Inherited-field write codegen fix (#208, commit `16434e5`) — subclass struct now prepends inherited fields, so GEPs for inherited fields share the parent's slot index. Follow-up (commit `0dda81b`) added the same walk to `BinaryOpExpression`'s ASSIGN slot-type lookup (mixed-width inherited fields were trampling neighbors with too-wide stores) and switched `CastExpression::generateCode` to `loadIfLValue` so `(T) obj.field` loads the value instead of ptrtoint'ing the GEP address.
- `String`-parameter codegen fix (#211, commit `b987e59`) — variable-size-field check now gates on `dynamic_pointer_cast<CajetaStruct>`, so a `String` field on a regular class is freely writable. Unblocked `Throwable.message: String`.
- `int8` / `uint8` wired as documented native types (commit `2a9655b`) — `INT8`/`UINT8` lexer rules and `CajetaType::init` registrations were missing, so `int8` lexed as IDENTIFIER and silently produced null types that segfaulted in struct layout.
- Unknown field types throw `CAJETA_ERROR_UNKNOWN_TYPE` at `visitFieldDeclaration` instead of silently leaking null (commit `ac122a6`) — turns the same segfault shape into a diagnostic with the user's original type-name token.

---

## Known gaps surfaced by the rollout

- **Drop chain head is a single global static, not TLS.** Documented in `MemoryModel.md` § Known gaps. Correct under today's single-carrier model (only one fiber executes at a time) but a multi-carrier pool would race on it. Drop entries themselves are already alloca'd in the fiber's own stack, so the only piece that needs TLS-promotion is the head pointer.
- **Task<T> instances aren't drop-tracked.** spawn mallocs a `CajetaTask` struct; nothing currently frees it. The R5-A/A' joins ensure the *task* completes, but the heap allocation leaks until process exit. Wiring `CajetaTask` into the drop chain is post-R5 cleanup.
- **`detach` is still a synchronous pass-through, not a real async fire-and-forget.** Today's `DetachExpression::generateCode` just inline-calls the inner expression. Real detach should enqueue a fiber that outlives the current scope; lands when the doc's "detach requires `#` captures" check is also in place.
- **No `Task<T>` exposed as a user-typeable template.** spawn produces a `Task<int32>*` at the LLVM level, but the user can't write `Task<int32>` as a variable type today (it's only synthesized at spawn sites). Some otherwise-natural test shapes (storing two task handles before either is awaited, e.g. to exercise true fiber-on-fiber lock contention) need this.

---

## Notes / open questions

- **Worker thread pool.** Today's executor is a single carrier OS thread. A pool would let multiple fibers run truly in parallel, but raises new questions: the global drop-chain head needs TLS promotion (above); the wake-all-parked policy in `__cajeta_task_complete` would scale badly with many concurrent awaits; the lock and scope code already takes the right per-object locks but hasn't been audited under concurrent carriers. Probably a v2 concern.
- **Function-body implicit scope: cost.** Each method call now pays one malloc + one free for its scope frame, even if the method never spawns. Profiling will tell if this matters; the optimization is straightforward (lazy frame alloc on first scope_register) and can land when measured.
