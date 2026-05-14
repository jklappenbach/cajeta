# Async Runtime — Implementation Status

Tracks the R1–R5 rollout of the async runtime described in `ThreadModel.md`. Counterpart to `ImplementationStatus.md` (which covers the MemoryModel + WireFormats rollout).

---

## Current status

**Phase R1–R5-A' complete.** Stackful fiber executor with cooperative yield, arg capture for spawn, async-aware locks, and structured-concurrency scope joins (both explicit `scope { }` and implicit function-body scope) are all in. R5-C and R5-D are blocked on the error model decision.

**Next:** implement the error model per `ErrorModel.md` (exception-hierarchy design, settled). R5-C and R5-D land on top of the error-model work.

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
- `ErrorModel.md` — drafted. Errors-as-values, `try` mandatory at call sites, `panic` for unrecoverables, multi-arm pattern catches. Commit `ab68857`.

---

## Blocked / pending

### Error model
**Spec settled** — `ErrorModel.md` uses an exception-hierarchy design (Unrecoverable/Recoverable, advisory `throws` clause, system default catch). Implementation work list is at the bottom of the doc. Roughly: stdlib `Throwable`/`UnrecoverableException`/`RecoverableException` → `throws` grammar → lint warning → runtime carries `Throwable*` not `int64` → system catch wrapping main + each fiber trampoline → `CajetaTask` exception slot + `await` re-raise.

### R5-C — Cancellation tokens
Now unblocked by the error-model decision. Surfaces as a `CancellationException extends RecoverableException`. Implementation:
- Each `Task` (or fiber) gets an atomic cancel flag.
- `__cajeta_task_cancel(task)` sets the flag.
- `__cajeta_task_wait` checks on resume and (via the new exception machinery) re-raises `CancellationException` into the awaiter's frame.
- Scope-level cancel iterates registered children and sets each one's flag.

### R5-D — Exception escalation through scope
Unblocked by the error-model decision. When `scope_exit` joins children, walk each child's `Task.exception` slot; if any child threw, cancel remaining siblings, wait for their unwinds, re-raise the first exception into the scope's containing frame.

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
