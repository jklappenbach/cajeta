# Cross-Carrier Stop-the-World Quiesce (Debugger CP6f-2d) — Spec

> Status: **DRAFT for review** (design skill). Specs the fix for the debugger's
> multi-carrier stop-the-world gap (CP6f-2d). Spans the runtime scheduler, the
> in-process `DebugController`, and the JIT/DAP host — all in this repo.
> Companions: [`docs/Debugging.md`](../Debugging.md) (DAP contract),
> `src/cajeta/dbg/DebugController.h`, `runtime/native/cajeta_runtime.c` (carrier
> pool + fiber registry), `src/cajeta/jit/CajetaJitHost.cpp` (`liveFibers`,
> `startDebugSession`), `src/cajeta/dap/DapServer.cpp` (`threads`/`stackTrace`).
> Plan: `agents/carrier-quiesce-plan.md`.

---

## 1. Definition

### 1.1 Purpose
Make a debugger stop **actually stop the whole program** under the multi-carrier
scheduler: when execution parks at a breakpoint (or armed exception), **all**
carrier threads must quiesce before the debugger inspects state, and **all** must
resume together on `continue`. Today only the single carrier that hit the
safepoint parks.

### 1.2 Problem (verified)
The runtime spawns a **carrier pool** — `runtime/native/cajeta_runtime.c` creates
one `__cajeta_carrier_loop` thread per core (`__cajeta_carriers[]`,
`__cajeta_carrier_count`), each with its own work-stealing deque. Fibers run on
these carriers **concurrently**. The debugger's stop model
(`DebugController::onSafepoint`) blocks **only the calling thread**. Consequences:

1. **Not stop-the-world.** When a breakpoint hits on carrier A, carriers B/C…
   keep running fibers. State the debugger reads can change underneath it; a
   "stopped" program is still executing.
2. **Registry enumeration race.** `JitDebugSession::liveFibers()` reads
   `__cajeta_dbg_fiber_count()` then loops `__cajeta_dbg_fiber_at(i)`, each
   taking/releasing `__cajeta_dbg_fiber_reg_mutex` separately
   (`cajeta_runtime.c:215-231`). Concurrent `register`/`unregister` on the
   still-running carriers shifts indices → TOCTOU (stale handles, missed/dup
   fibers).
3. **Stale doc.** The `DapServer.cpp` `threads` comment claimed "the carrier is
   parked while we enumerate, so the registry is stable" — false for >1 carrier.
   (Corrected to a FIXME pointing here.)

### 1.3 Scope (v1)
- A **global stop request** all carriers (and the entry/main thread) observe at
  safepoints and at scheduler hand-off, so any breakpoint parks the whole pool.
- A **quiesce barrier**: the debugger inspects only after every active carrier is
  confirmed parked (or accounted for as not-executing-Cajeta), within a bounded
  wait.
- **Resume-all**: `continue` releases every parked carrier together.
- An **atomic fiber-registry snapshot** accessor replacing the count()+at() loop.
- Correct interaction with the **exception** stop path (`onException`) and the
  **entry/main** thread.
- Handling for carriers that are **idle** (no fiber) or **blocked in a native
  call** (can't reach a safepoint promptly).

### 1.4 Non-goals (v1)
- Per-fiber / single-stepping-one-fiber-while-others-run (the spec keeps the
  stop-the-world model; `cajeta:setPauseMode` single-fiber is a later item).
- Preempting a carrier stuck in a long blocking syscall (cooperative safepoints
  only; such carriers are reported as un-quiesced within the bound, not forced).
- Changing the DAP wire contract (`threads`/`stackTrace`/`continue` unchanged);
  this is a backend-correctness fix.

---

## 2. Mechanism (how it works)

### 2.1 Global stop state
A process-global, debug-only stop coordinator (owned by the runtime, driven by
`DebugController`):
- `stop_requested` (atomic flag/generation) — set when any armed safepoint or
  armed exception fires.
- `parked_count` / `expected_count` — how many carriers have parked vs how many
  must, guarded by a mutex + condvar.
- Zero cost when no breakpoint/exception is armed and no debug session is active
  (gated like the existing safepoint emission, `emitGuard`).

### 2.2 Convergence
1. Carrier A hits an **armed** `__cajeta_dbg_safepoint(locId)`: records the
   *primary* `StopEvent` (reason=Breakpoint), sets `stop_requested`, increments
   `parked_count`, and **blocks** (as today, via `DebugController`).
2. Every other carrier, at its **next** safepoint, observes `stop_requested`,
   records itself as a *secondary* (quiesced, no breakpoint), increments
   `parked_count`, and blocks.
3. The **scheduler hand-off** path (a carrier picking the next ready fiber from
   its deque / the global queue) also checks `stop_requested` and parks **before**
   resuming a fiber — so a carrier between fibers can't start new Cajeta work
   while stopped.
4. **Idle** carriers (parked on the scheduler's no-work condvar) are already
   not executing Cajeta; they are counted as quiesced and must not pick up work
   until resume (the hand-off check in (3) guarantees this).

### 2.3 Quiesce barrier (debugger side)
`DebugController::waitForStop` (and the DAP `stopped` emission) returns only once
`parked_count == expected_count` **or** a bounded `quiesce_timeout` elapses. The
returned stop reports which carriers quiesced; any that didn't (e.g. blocked in a
native call) are flagged so the DAP layer can mark their fibers "running"/
unavailable rather than presenting stale state.

### 2.4 Inspection
Only after the barrier do `threads`/`stackTrace`/`variables`/the memory accessors
run. Fiber enumeration uses the new **atomic snapshot** (§2.6).

### 2.5 Resume-all
`DebugController::resume` clears `stop_requested`, then signals **all** parked
carriers (primary + secondaries + idle) to continue; `parked_count` resets. No
carrier resumes a fiber until the flag is cleared (the §2.2.3 hand-off check).

### 2.6 Atomic registry snapshot
Add `__cajeta_dbg_fiber_snapshot(handle_out[], max) -> count` (and the derived
id/frame_top/state read either from the held handles or a struct-of-arrays),
copying the registry under a **single** `__cajeta_dbg_fiber_reg_mutex` hold.
`liveFibers()` uses it instead of count()+at(). Correct even independent of the
quiesce (removes the TOCTOU at the source).

---

## 3. Use cases
- **3.1** As a developer debugging a multi-fiber program, when a breakpoint hits
  in one fiber, then **all** carriers stop and no other fiber advances while I
  inspect.
- **3.2** As a developer, when I open the Threads/Fibers view at a stop, then the
  fiber list is a consistent snapshot (no missing/duplicated/stale fibers) even
  though fibers were being spawned/finished right up to the stop.
- **3.3** As a developer, when I `continue`, then all carriers resume together and
  the program proceeds normally.
- **3.4** As a developer, when a breakpoint hits, then a fiber on another carrier
  that was *about to* hit the same or another breakpoint parks cleanly rather
  than racing past or double-stopping.
- **3.5** As a developer, when a carrier is blocked in a long native call at the
  stop, then it is reported as un-quiesced (its fiber shown as running/
  unavailable) within the bounded wait, and the rest of the program is still
  inspectable — never a hang.
- **3.6** As a developer hitting an **armed exception**, then the same whole-pool
  quiesce applies before I inspect the throwing frame.

---

## 4. Non-functional requirements
- **4.1 No deadlock.** Lock ordering for the stop coordinator vs the
  carrier/deque/registry mutexes is defined and acyclic; the barrier uses a
  bounded wait so a stuck carrier can never hang the debugger.
- **4.2 Zero cost off-path.** No added cost when no session is active / nothing
  armed; the safepoint and hand-off checks are a single relaxed-atomic load on
  the hot path, gated by the existing debug-info/safepoint guard.
- **4.3 Determinism.** Exactly one *primary* breakpoint stop per round even if
  two carriers hit armed safepoints near-simultaneously (the second becomes a
  secondary); the primary is well-defined (first to set the flag).
- **4.4 Correctness independent of timing.** The atomic snapshot (§2.6) holds
  even without the quiesce; the quiesce holds regardless of carrier count
  (including 1).
- **4.5 Cross-platform.** POSIX (pthreads) and Windows carrier paths both honored
  (`cajeta_runtime.c` already #ifdefs both).

---

## 5. TDD / verification (against `cajeta_debug_test`)
- **5.1** A multi-fiber program with a breakpoint in one fiber: assert that after
  the stop, a counter advanced by other fibers does **not** change across a fixed
  wait (no progress while stopped), then advances after `continue`.
- **5.2** Spawn/finish fibers in a tight loop while repeatedly hitting a
  breakpoint; assert `liveFibers()`/snapshot is always internally consistent
  (count matches returned handles; no nulls; ids unique).
- **5.3** Two fibers hit armed safepoints near-simultaneously: assert exactly one
  primary breakpoint stop, the other quiesced, and both resume on `continue`.
- **5.4** Bounded quiesce: a fiber blocked in a simulated long native call →
  `waitForStop` returns within `quiesce_timeout` flagging it un-quiesced, no hang.
- **5.5** Single-carrier / no-fiber programs still stop/resume exactly as before
  (regression).

---

## 6. Risks
- Touching the carrier scheduler hot path (hand-off) and the stop rendezvous is
  **deadlock-sensitive**; §4.1 lock-ordering discipline and bounded waits are
  mandatory. Each TDD case in §5 must pass under a thread sanitizer where
  available.
