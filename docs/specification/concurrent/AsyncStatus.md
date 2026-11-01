# Async Runtime — Implementation Status

This page tracks the R1–R5 rollout of the async runtime described in `docs/specification/concurrent/Concurrency.md`. It is the counterpart to `history/ImplementationStatus.md`, which covers the now-complete MemoryModel rollout.

<!-- SYNC NOTE: this file is mirrored at cajeta-docs-site/src/pages/docs/AsyncStatus.md
     (the published-site copy, which adds Astro frontmatter and uses bare doc names).
     Keep the body content of the two in sync when editing either. -->


---

## Current status

Phases R1 through R9 and error-model v1 are complete. The full
structured-concurrency story works end to end. It covers stackful fibers on a
carrier pool with per-carrier work-stealing deques, scope joins, cancellation,
exception escalation, atomics, timers, and the async I/O reactor. The error
model has the stdlib Throwable hierarchy in `package cajeta.error;` with `cause`
chaining, the `throws` clause grammar with an advisory lint that is aware of
try/catch coverage, the runtime exception path on `void*`, a Task<T> exception
slot with await re-raise, and stack-trace capture at throw sites with
auto-print on uncaught.

> **Scheduler reality.** This document is the authoritative source of truth for
> the scheduler. The executor is a carrier pool with Chase–Lev work-stealing
> deques per carrier. The default carrier count is `min(nproc, 4)`, and
> `CAJETA_CARRIERS=N` overrides it, clamped to `[1, 16]` and read once at first
> spawn. The pool is multi-carrier by default, so spawned tasks run with real
> wall-clock parallelism. The multi-carrier deadlocks of earlier releases no
> longer reproduce across the async/threading suite at the default count. The
> one limit is that a fiber is pinned to the carrier that first ran it, because
> cross-carrier resume of a suspended fiber is unsolved. Parallelism therefore
> comes from fanning out across distinct spawned fibers, since a single fiber
> never migrates. Set `CAJETA_CARRIERS=1` for deterministic single-carrier runs.
> A few sync-primitive fast paths and the stdlib `Mutex` docstring still describe
> the old "single carrier" discipline. The timer wheel ships as
> `__cajeta_task_wait_timeout` plus a sorted-list timer thread, and it backs
> `cajeta.time.Duration`,
> `Tasks.withTimeout<R>`, and `Tasks.withDeadline<R>`. The async I/O reactor
> ships on Linux epoll, with the `Cajeta.epoll*` and `Cajeta.eventfd*`
> intrinsics for non-blocking fd registration and wait. Atomics ship as
> `cajeta.concurrent.AtomicInt32/64` (R8.1).

R5-C and R5-D have shipped. They are named here precisely because `specs/Features.md` S-805 long read "designed":
- [x] R5-C cooperative cancellation. The scope sets each surviving child fiber's `cancel_with` marker to the trigger Throwable, which is the first thrower, and `__cajeta_task_wait` re-raises that trigger on the next park-resume (commit `fa7c7f8`). There is no `CancellationException` type, because the trigger itself propagates.
- [x] R5-D scope exception-escalation. `scope_exit` and `scope_exit_to` join children, cancel surviving siblings with the trigger on the first non-null exception slot, and then re-raise the first throw into the containing frame.

All post-v1 polish items have shipped:
- [x] Recoverable/Unrecoverable distinction via vtable type-check (#210), commit `ea5ca6e`.
- [x] Inherited-field write codegen fix (#208), commit `16434e5`. The `0dda81b` follow-up covers the inherited-field ASSIGN slot width and the orthogonal `(T) obj.field` cast-load bug surfaced by stress testing.
- [x] `String` parameter codegen fix (#211), commit `b987e59`. `Throwable.message` is now `String`.
- [x] Lint refinements (#209), commit `996cf21`. The uncaught-throws lint now walks an enclosing-try stack and suppresses warnings when any catch arm covers the throw, with supertype awareness. The annotation generalized to `@SuppressLint("id")` in `e3d238e`.

Still open:
- A few smaller known gaps and v2 concerns remain. See *Known gaps* below.

---

## Plan: Task<T> as user-typeable template  ✅ complete

Today the compiler synthesizes `Task<T>` at every spawn site, through `CajetaTask::getOrCreate(module, T)` called from `SpawnExpression::resolveTypes`. You can now also write `Task<int32>` as a variable type, so the canonical workload of storing two task handles before awaiting either is expressible.

### Punch list

- [x] **20.1** Type resolution. `Task<int32>` as a `typeType` already parses through `classOrInterfaceType`, since the grammar accepts type arguments uniformly. The miss was in `CajetaType::fromContext(TypeTypeContext*)`. When the bare name `Task` is followed by `typeArguments`, it now routes through `CajetaTask::getOrCreate(module, T)` instead of looking `Task` up as a user class. The special-case branch sits ahead of the general template path, so no fake "Task" template class is needed to satisfy `isTemplate()`.
- [x] **20.2** Ownership transfer, option (a), auto-promotion on assignment. A `llvm::Value* dropEntry` field on `SpawnExpression` is populated when the push happens. `LocalVariableDeclaration`'s initializer path detects a `SpawnExpression` RHS via `dynamic_pointer_cast` and emits `__cajeta_drop_mark_inactive` on the entry, so the local's own class-instance drop becomes the canonical owner. A bare-statement `spawn foo();` is unaffected. There is no assignment and no inactivation, and the spawn's drop still fires at scope exit. BinaryOpExpression ASSIGN did not need the same treatment, because today's tests rebind via re-declaration rather than via plain `=`. This will be revisited when that shape lands.
- [x] **20.3** `await someTaskLocal`. Two pieces had to land. The first is a defensive `inner->resolveTypes(module)` re-resolve at codegen time in `AwaitExpression::generateCode`, because the pre-pass runs before the local is in scope and leaves resolvedType null, which sent the dispatch into the sync-compat branch returning the raw Task ptr. The second is a `SpawnExpression` preempt in `loadIfLValue`, so `Task<int32> t = spawn foo();` does not try to load the 24-byte Task value through an 8-byte ptr slot. For class types the value is the pointer.
- [x] **20.4** Package question. The decision is to keep `CajetaTask` package-free, as the comment at `CajetaTask.cpp:17` already says. Users write the bare name `Task<int32>`, and type resolution finds it directly through the synthesized-type path. This will be revisited when `cajeta.concurrent.*` actually lands.
- [x] **20.5** Tests in the new file `test/parser/TaskTypingTests.cpp`: `declareAwaitOneTask`; `twoHandlesStoredThenAwaited`, the canonical use case; `differentTaskElementTypes`, where Task<int32> and Task<int64> get distinct value-field widths; `declaredTasksDropOncePerLocal`, where the drop count is 2 for two declared tasks, proving option (a)'s inactivation; `awaitedTaskStillDropsOnceAtScopeExit`.

### Race fix surfaced en route (commit `ca2b0d7`)

While probing item 20.2, `SpawnDropTests` flaked about 20% of the time as a suite. A probe isolated `free(task)` in `__cajeta_task_drop` as the trigger. The root cause was unrelated to the typing work itself. `ScopeStatement::generateCode` emitted `scope_enter → body → DROPS FIRE → scope_exit` for explicit `scope { }` blocks, but `__cajeta_scope_exit` walks every registered task's `exception_addr`, which is a pointer into the task allocation, so the drop's `free(task)` ran before the scope_exit read through it. That was a use-after-free the allocator usually masked. The fix reorders `ScopeStatement` to manage its block's drop frame manually and emit drops after `__cajeta_scope_exit`. The method body's fall-through path already had this invariant, with scope_exit_to before emitOwnerDrops, so only the explicit-scope path was wrong.

### TLS-promote drop-chain head + exception chain head  ✅ complete

`MemoryModel.md` § Known gaps and `AsyncStatus.md` both flagged the drop and exception chain heads as single static globals, "correct under today's single-carrier model but a multi-carrier pool would race." That framing under-stated the actual risk. The carrier thread is already a separate OS thread from main, so even single-carrier work races on the globals when main and the carrier are both active.

Promotion landed on the same pattern `scope_top` already uses, with per-fiber slots for code running inside a fiber and a `__thread` TLS slot for the main thread:

- `cajeta_fiber` gained `drop_top` and `exc_top` fields, zero-initialized in `__cajeta_task_run`.
- `__cajeta_main_drop_top` / `__cajeta_main_exc_top` are `__thread`-qualified per-OS-thread slots.
- `__cajeta_drop_top_ptr()` / `__cajeta_exc_top_ptr()` helpers return the right slot based on `__cajeta_current_fiber`, mirroring `__cajeta_scope_top_ptr()`.
- `__cajeta_drop_count` stays a process-global, since it is test observability across both threads, but increments and the get/reset accessors now go through `__atomic_*_n(..., __ATOMIC_SEQ_CST)` so drops fired on the carrier are visible to main.
- All call sites (`drop_push`, `drop_pop_run`, `exc_push`, `exc_pop`, `throw`, `get_thrown`) read/write through the helpers.

The new test `SpawnDropTests.carrierDropsAccountedSeparately` has a spawned method declare its own `int32[]` local. The spawned method's drop fires on the carrier's per-fiber chain, and main's two `Task<int32>` drops fire on main's TLS chain. The atomic counter sums them. Without TLS promotion, the carrier's push/pop would corrupt main's chain through the shared global head.

### Real `detach` (fire-and-forget) + `#`-captures rule  ✅ complete

`DetachExpression::generateCode` was previously a sync passthrough. It only called `inner->generateCode(module)`, so `detach foo();` ran inline on the caller's thread and was indistinguishable from a bare call. The fire-and-forget semantics `docs/specification/concurrent/Concurrency.md` describes were not actually implemented.

The spec was tightened first, in `docs/specification/concurrent/Concurrency.md` § detach Semantics (v1). The inner must be a method call. Captures must each be `#`-transferred, primitive, or a fresh `heap T(...)`. The Task wrapper leaks for the process lifetime. Exceptions in detached bodies are captured to the Task's exception slot but never observed, since there is no awaiter and no scope. The leak is the explicit "use sparingly" trade-off.

The implementation reuses `SpawnExpression`'s full trampoline and fiber-enqueue lowering. A new `detachMode` flag on `SpawnExpression` gates the two pieces detach skips:

- `__cajeta_scope_register` is skipped, so `scope_exit` will not wait on this Task.
- The Task's drop-chain push is skipped. No scope owns the Task, so it leaks.

`DetachExpression::generateCode` resolves the inner's argument types and runs `enforceDetachMoveOnlyCaptures`, the `#`/primitive/`new` check that throws `CAJETA_ERROR_DETACH_BORROW_CAPTURE` on violation. It then constructs a transient `SpawnExpression` wrapping the same `MethodCallExpression` with `detachMode=true` and delegates to it, so the trampoline has a single source of truth.

An auxiliary fix landed with it. `SpawnExpression`'s value-store path now skips when the inner returns void, through an `isVoidTy()` check, and `CajetaTask::CajetaTask` substitutes an `i8` placeholder for the value slot when `elementType` is void, because LLVM forbids storing void or putting it in an aggregate field. This was previously latent because all spawn tests used int-returning inner functions, and it surfaced when `detach foo();` runs an `async void foo()`.

New tests in `test/parser/DetachTests.cpp` cover four cases: a no-capture detach returns immediately; primitive captures work; `#`-transferred class captures work; a bare class-typed identifier, which is a borrow capture, is rejected at compile time. The fresh-allocator case, `heap T()` inline as an argument, is documented as a Known gap rather than tested directly. The underlying NewExpression-as-method-call-argument codegen path is broken even for non-detach calls, since `consume(heap Payload())` crashes, so the detach-specific check accepting `NewExpression` will light up the moment that gap is fixed.

### Out of scope for this rollout

- Task<T> as a field type on a user class.
- Task<T> as a method return type (would require the async lowering to materialize a Task at the return site).
- Task<T> as a type argument of another template (`Box<Task<int32>>`). This should fall out of monomorphization, but it is not tested.
- Move-out via `#t`, transferring a Task local to a method. The drop chain machinery supports it, but it is not a Task-specific concern. The general move-out rules will cover it when those tests are added.

### Ownership-transfer model — why option (a)

`MemoryModel.md` § Borrow / transfer rules says *"Auto-promotion for fresh `new`. An anonymous `heap T(...)` expression in transfer position promotes implicitly. ... The temporary is an unnamed owner with no prior identity, so promotion has no use-after-move risk."* Spawn is a fresh allocator with no prior identity, so the same rule applies. The drop-chain primitive `__cajeta_drop_mark_inactive` already exists for `#`-move, and option (a) invokes it from a new call site without inventing a mechanism.

Two alternatives were considered and rejected:
- *(b) Local skips its own drop when the initializer is a Spawn.* This is localized, but it breaks `#`-move-out of Task locals, because with no entry to deactivate, ownership cannot be transferred out of the local later. It makes Task a second-class citizen for the move syntax.
- *(c) Spawn never pushes a drop; bare-statement spawn synthesizes an anonymous local.* This is the cleanest "every owner has a name" story, but it touches `ExpressionStatement` or the parser. That is a larger blast radius than the punch list above warrants.

---

## Completed

### Foundation (pre-R1)
- Grammar additions: `async`, `await`, `spawn`, `scope`, `detach` keywords.
- AST nodes: `AwaitExpression`, `SpawnExpression`, `DetachExpression`, `ScopeStatement`.
- Async modifier carried on `Method` (`ASYNC` bit in `Modifiable`).
- Sync-lowering MVP. All five keywords parse and codegen as pass-throughs, and the surface syntax is stable. Commit `2a5a11b`.

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
- The decision is stackful (Java 21 virtual thread model) over stackless state machines. This removes function coloring and needs no async-fn codegen transformation.
- `ucontext.h`-based fiber primitive with ~64 KB per-fiber stack.
- Carrier OS threads (default `min(nproc, 4)`) each run many fibers cooperatively; fibers are pinned to their home carrier.
- `await` from inside a fiber parks (swap to carrier); main-thread `await` OS-blocks on a condvar.
- `__cajeta_task_complete` wakes all parked fibers. This is a polling wake, and per-task wait queues are a future optimization.
- docs/specification/concurrent/Concurrency.md updated to reflect the stackful decision and document the global drop-chain-head as a known gap.
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
- R5-B (parse-time error for spawn-without-scope) is subsumed, because every spawn is structurally inside some scope.
- Commit `a4bedb3`.

### Design spec
- `ErrorModel.md` was drafted with errors-as-values, `try` mandatory at call sites, `panic` for unrecoverables, and multi-arm pattern catches. Commit `ab68857`. It was subsequently rewritten around an exception-hierarchy design (commit `6fe6cb6`), and the rest of the bullets below describe the shipped version.

### Error model — implementation (v1 complete)
- Stdlib `Throwable` / `Exception` / `RecoverableException` / `UnrecoverableException` classes, originally in `package cajeta.lang;`, moved to `package cajeta.error;` for discoverability and reuse. `Exception` carries a typed `Throwable cause` for chain-of-causality.
- `throws` clause grammar + AST (#200), advisory `uncaught-throws` lint warning (#201), stack-trace capture at throw sites (#203).
- Runtime exception path migrated to `Throwable*` on `void*` carrier; system default catch wraps main + each fiber trampoline. `CajetaTask` gained an exception slot; `await` re-raises into the awaiter.
- Recoverable/Unrecoverable distinction at system catch (#210), a vtable type-check against the `__cajeta_unrecoverable_vtable_marker` global published by `Compiler::parse()` after stdlib load.
- R5-C cancellation (commit `fa7c7f8`). A scope-level cancel walks registered children and sets each surviving fiber's `cancel_with` marker to the trigger Throwable, and the next await re-raises it. There is no `CancellationException` class, because the trigger itself propagates.
- R5-D exception escalation through scope. `scope_exit` joins children, cancels siblings on the first throw, and re-raises into the containing frame.
- Comprehensive lint system with rule IDs and `@SuppressLint("id")` (commits `ded5897`, `e3d238e`), plus try/catch coverage awareness for `uncaught-throws` (#209, commit `996cf21`). The lint walks an enclosing-try stack and suppresses warnings when any catch arm catches the throw, with supertype awareness via `getSuperClasses()`.
- Inherited-field write codegen fix (#208, commit `16434e5`). The subclass layout now prepends inherited fields, so GEPs for inherited fields share the parent's slot index. The follow-up (commit `0dda81b`) added the same walk to `BinaryOpExpression`'s ASSIGN slot-type lookup, since mixed-width inherited fields were trampling neighbors with too-wide stores, and switched `CastExpression::generateCode` to `loadIfLValue` so `(T) obj.field` loads the value instead of ptrtoint'ing the GEP address.
- `String`-parameter codegen fix (#211, commit `b987e59`). The variable-size-field check now gates on `dynamic_pointer_cast<CajetaStruct>`, so a `String` field on a regular class is freely writable. This unblocked `Throwable.message: String`.
- `int8` / `uint8` wired as documented native types (commit `2a9655b`). The `INT8`/`UINT8` lexer rules and `CajetaType::init` registrations were missing, so `int8` lexed as IDENTIFIER and silently produced null types that segfaulted in class layout.
- Unknown field types throw `CAJETA_ERROR_UNKNOWN_TYPE` at `visitFieldDeclaration` instead of silently leaking null (commit `ac122a6`). This turns the same segfault shape into a diagnostic with the user's original type-name token.

---

## R7 — Sync primitives ✅ complete

Beyond R4's Lock, the threading package ships:

- `cajeta.concurrent.Mutex<T>` has a closure/scoped API (`withLock((T) -> #T fn)`, `get()`) and no lvalue value handle. R7-A..E.
- `cajeta.concurrent.RwLock<T>` is multi-reader / exclusive-writer, with a `read() -> T` snapshot and `withWrite((T) -> #T fn)`.
- `cajeta.concurrent.Semaphore` is counting, with `acquire`/`release`, the scoped `withPermit(() -> void fn)`, and `availablePermits()`.
- Wait-for-condition is folded into `Mutex.withLockWhen((T) -> boolean cond, (T) -> #T fn)`, backed by the runtime's `Cajeta.condvar*` intrinsics. There is no standalone `ConditionVariable`/`CondVar` class.
- `cajeta.concurrent.Channel<T>` is a bounded MPMC ring buffer with its capacity set at construction. `send` blocks while full, and `receive` returns a stack `Optional<T>` that is present while items remain and empty once closed and drained. v1 targets primitive/value T. Heap-class T items that are buffered but unreceived are not dropped on Channel destroy, so drain explicitly via a `receive()` loop before the drop. R7-F.

## R8 — Multi-carrier scheduler + atomics ✅ complete

- **R8.1** adds `cajeta.concurrent.AtomicInt32` / `AtomicInt64` value types with `load`, `store`, `fetchAdd`, `compareAndSet` (seq_cst), plus fixed-ordering variants (`loadRelaxed`/`loadAcquire`, `storeRelaxed`/`storeRelease`, `fetchAddRelaxed`, `compareAndSetAcquire`).
- **R8.2** replaces the single shared ready queue with Chase–Lev work-stealing deques per carrier. Local push/pop is wait-free for the owner, and other carriers steal from the public end.
- **R8.3** adds the multi-carrier pool. `CAJETA_CARRIERS=N` (1 ≤ N ≤ 16) sets the pool size, and the default is `min(_SC_NPROCESSORS_ONLN, 4)`. The TLS-promoted per-fiber drop and exception chain heads keep the unwind walks per-carrier.

## R9 — Timers + async I/O ✅ complete

- **R9.1** adds the `__cajeta_task_wait_timeout(done_addr, deadline_ns)` runtime primitive. It parks the calling fiber on a sorted timer list and wakes at the deadline or when done flips, honoring R5-C cancellation on wake.
- **R9.2** adds the `cajeta.time.Duration` value type with `ofNanos`/`ofMillis`/`ofSeconds`/`ofMinutes` factories and the `toNanos` accessor.
- **R9.3** adds `cajeta.concurrent.Tasks.withTimeout<R>(Duration, Task<R>) -> Optional<R>` and `withDeadline<R>(int64 deadlineNanos, Task<R>) -> Optional<R>`. The Optional reports present-or-timeout. On timeout the body is cooperatively cancelled (`Cajeta.taskCancel` → `__cajeta_fiber_cancel`), bodies that yield short-circuit on the next park, and bodies without yield points run to natural completion. A legacy `withTimeoutInt32` specialization remains for callers already pinned to it.
- **R9.4** adds the Linux epoll-based I/O reactor, with the `Cajeta.epollCreate`, `Cajeta.epollAdd`, `Cajeta.epollWait`, `Cajeta.eventfdCreate`, `Cajeta.eventfdSignal`, `Cajeta.eventfdConsume`, and `Cajeta.fdClose` intrinsics. The carrier hosts the reactor thread, and fibers park on fd-readiness events.
- **R9.5** adds `cajeta.concurrent.Tasks.runBlocking<R>(() -> R body) -> R`, the sync→async bridge. It spawns `body` on the carrier pool, blocks the calling OS thread on the body's done flag, and returns its result. The runtime's `__cajeta_task_wait` already condvar-waits when there is no current fiber, so a plain non-async `main` can drive async work without itself being `async`. v1 covers primitive / value-return R via the spawn-of-lambda ABI, and heap-class return callers keep using the explicit `await spawn body()` form. It also surfaces `Fiber.sleep(Duration)`, built atop `__cajeta_task_wait_timeout` against a sentinel done-flag, as the underlying cooperative-sleep primitive.
- **R9.6** adds `cajeta.concurrent.Tasks.selectReceive<T>(Channel<T>[]) -> Optional<SelectResult<T>>`, a Go-style `select`-receive across multiple channels. It returns `present(SelectResult{index, value})` when one of the channels produces, and `empty` once every channel has been closed and drained. The lowest index wins on simultaneous readiness, which is deterministic. The v1 implementation polls each channel via `Channel.tryReceive()` / `Channel.isClosed()` and backs off between passes via `Fiber.sleep`, exponentially from 100us to a 10ms cap. The fiber parks on the timer wheel instead of spinning, so the carrier is free to run other fibers. A future revision can park on per-channel waiter lists for zero-poll-cost wake.
- **R9.7** adds the `cajeta.concurrent.AsyncIterator<T>` interface, a minimal iteration contract. `Optional<T> next()` returns present until the source is exhausted and empty thereafter. The user-side loop is explicit (`while ((opt = iter.next()).isPresent()) ...`), and a `for (T x in iter) { ... }` desugaring is the planned v2.1 surface. Multi-call iteration is sound through both concrete-typed and interface-typed receivers. The interface decl's prototype adopts the sret ABI when its return type is a value-shape class, which matches the impls' sret signature, and the dispatch site swaps the iface body for the data pointer at the `this` slot after sret rather than blindly at position 0.

## Language surface for async-driving stdlib ✅ complete

- **spawn-of-lambda.** `SpawnExpression` accepts a function-typed scope field, whether a local, a parameter, or a capture, as the inner call. This lets `Tasks.runWith(...)`, or anything that takes a `() -> R body` parameter, spawn the lambda body internally. v1 restricts to heap-ownership / primitive return. sret value-return closures are rejected with a clear error, because the slot would need to outlive the worker frame.

---

## Known gaps surfaced by the rollout

- **Multi-receiver Optional chained sret call.** Three sequential stack `Optional<T>` locals from sret-return calls where the third's terminal is `.orElse(literal)` SIGSEGVs, while two locals or three locals with isPresent/get is fine. The channel-side workaround in the tests is to guard with `isPresent`. It is tracked separately as a stack-Optional codegen edge case.
- **Channel<T> heap-class drop refinement.** Items buffered in the ring buffer at Channel destruction time do not run their per-element drop, because the array frees without freeing its elements. The workaround is to drain via a `receive()` loop before the drop. v2.

---

## Notes / open questions

- **Function-body implicit scope cost.** Each method call now pays one malloc and one free for its scope frame, even if the method never spawns. Profiling will tell if this matters. The optimization is straightforward, a lazy frame alloc on the first scope_register, and can land when measured.
