# Exception-Throw Subprocess Hang — Fix Plan (CP6f-3c)

Status: **open bug**, root cause not yet fixed. This document captures the
diagnosis so the fix can be picked up cold.

## Symptom

Any Cajeta program that **throws an exception** hangs when run under a spawned
`cajeta dap` process (the IDE-attached / out-of-process debug path). The program
thread never finishes: the DAP server's `runToStopOrExit` enters its poll loop
and spins forever — `DebugController::waitForStop` keeps timing out (nothing
parks) and `JitDebugSession::isFinished()` never flips to true.

Surfaced by the CP6f-3 exception-breakpoint integration test
(`CajetaDebugSessionIntegrationTest.exceptionBreakpointStopsAtThrow`), which is
currently `@Ignore`'d with a pointer to this document.

## Crucial scoping: this is NOT an exception-breakpoint bug

Verified by temporarily setting `exceptionBreakpoints = false` in the
integration test: an **unarmed, caught** throw *also* never terminates under the
subprocess. So break-on-throw is not implicated — **any throw** hangs the
subprocess debug path. The CP6f-3 exception-breakpoint feature merely happened
to be the first automated test to throw inside a spawned `cajeta dap`.

## What works (so the fault is narrow)

- **In-process** the identical scenario PASSES: `debug-tests`
  `DapServerSession.ExceptionBreakpointStopsAtThrow` JIT-runs the same
  try/throw/catch program, parks at the throw with `reason="exception"`, resumes,
  and exits 42 — in ~4.8 s. So the DAP server, the runtime exception hook
  (`__cajeta_throw` → installed handler), `DebugController`, and the JIT host
  wiring are all correct.
- The plugin wire is unit-tested (`CajetaDebugSessionTest.setExceptionBreakpoints*`).
- **Other subprocess debug operations are fine**: line breakpoints, conditional
  breakpoints, the fibers view (threads + per-fiber stacks), variables, and
  `setVariable` all work against the spawned `cajeta dap`. Only the *throw path*
  hangs out-of-process.

So the divergence is specifically: **in-process JIT throw works; the same JIT
throw inside the `cajeta dap` subprocess hangs.**

## Ruled out

- **Stale binary / build freshness** — reproduced after forced clean rebuilds;
  confirmed `cajeta.exe` and the embedded `cajeta_runtime.bc` postdate the source.
- **Missing / dead-code-eliminated symbols** — `__cajeta_throw`,
  `__cajeta_dbg_set_exception_handler`, and the safepoint equivalents are all
  present (`T`) in the embedded bitcode; host `jit->lookup` resolves all four
  into the JIT module.
- **`static` vs external handler global** — making
  `__cajeta_dbg_exception_handler` external (single definition) changed nothing.
- **Arm-after-start race** — exceptions are now armed inside `startDebugSession`
  *before* the program thread starts (`armExceptions` param). Didn't fix it.
- **`backtrace()` / stack-trace capture** — `__cajeta_throw` calls
  `__cajeta_trace_record` (which calls `backtrace(3)`) first. Disabling capture
  for debug sessions (`__cajeta_set_stack_trace_capture(0)` in
  `startDebugSession`) did NOT fix the hang. (The call was kept anyway — it is
  correct behavior for a debug session, since the debugger supplies the stack.)

## Key diagnostic facts

- Host-side stderr DIAGs (reliable from the subprocess) showed: `runToStopOrExit
  ENTER` prints, but neither `got stop` nor `finished` ever prints → the poll
  loop spins; the program thread is genuinely stuck, not merely slow.
- The host exception trampoline (`exceptionTrampoline` in `CajetaJitHost.cpp`)
  fires **in-process** (observed `throwable=0x63` = 99) but **never** in the
  subprocess → in the subprocess the JIT'd `__cajeta_throw` does not reach the
  installed handler; control is lost earlier in the throw path.
- The entry runs on `startDebugSession`'s spawned `std::thread`, in both the
  in-process test and the subprocess — so "throw on a non-main thread" alone is
  not the trigger (the in-process test does it too and passes).

## Self-inflicted trap to avoid when debugging this

Do **not** add `fopen`/`fprintf`/`fclose` (any CRT file I/O) diagnostics *inside
the JIT'd runtime copy* (`runtime/native/cajeta_runtime.c` functions that the
JIT'd program calls, e.g. `__cajeta_throw`). Doing so **hangs the JIT** — it
broke the previously-passing in-process test until removed. Probe JIT'd code via
**host-side stderr** (in `CajetaJitHost.cpp` trampolines / DAP server) or a
process-global **atomic counter** the host reads back. CRT file I/O from JIT'd
code is itself a hang source and will confound the investigation.

## Likely area / hypotheses to test next

The throw path under `cajeta dap` differs from in-process only in that it runs
inside the standalone `cajeta dap` executable's process. Candidate causes, in
rough priority:

1. **`setjmp`/`longjmp` across the JIT/host boundary in the subprocess.**
   `__cajeta_throw` ends in `longjmp((*excTop)->buf, 1)` back to a `setjmp` the
   compiler emitted inline in JIT'd code. If the exception-frame chain
   (`__cajeta_exc_top_ptr`, a `__thread` selector) resolves to a different copy
   or a different thread's TLS in the subprocess, the `longjmp` target/`excTop`
   is wrong and control is lost → no park, no return. Check that
   `__cajeta_exc_push` (emitted in JIT'd try-entry) and `__cajeta_throw` agree on
   the same per-thread `exc_top` in the subprocess. Mirrors the known two-copies
   / TLS-selector hazard already documented for `dbg_top`/`scope_top`.
2. **TLS / `__thread` selector divergence.** The runtime uses `__thread` slots
   with selector functions (`__cajeta_exc_top_ptr`, `__cajeta_drop_top_ptr`,
   `__cajeta_dbg_top_ptr`) chosen by whether `__cajeta_current_fiber` is set. A
   throw in `main` runs on the program thread (not a fiber). Confirm the
   subprocess resolves these to the program thread's TLS, not a stale/foreign
   one.
3. **stdout contention.** `cajeta dap` writes DAP frames to **stdout**. If any
   part of the throw path writes to stdout (it shouldn't for a *caught* throw —
   `__cajeta_emit_uncaught` only runs when `*excTop == null`), it would corrupt
   the protocol or block. Verify the caught-throw path emits nothing to stdout.

## Suggested next steps

1. Reproduce minimally outside the plugin: drive a spawned `cajeta dap` over
   stdio (a small harness, **not** Python — earlier Python framing was
   unreliable; prefer a tiny C++/Kotlin driver or the existing
   `DapClientIntegrationTest` style) with the try/throw/catch program and
   confirm the hang.
2. Instrument host-side only (per the trap above): log in `exceptionTrampoline`
   and add an atomic counter bumped at the *top* of a host shim that the JIT
   calls, to pinpoint whether `__cajeta_throw` is entered at all in the
   subprocess vs. lost at the `longjmp`.
3. Bisect the throw path with host-observable signals: does `__cajeta_exc_push`
   run? Is `*excTop` non-null at the throw? Does the `longjmp` land?
4. Once the cause is found, fix it, **delete the `@Ignore`** on
   `exceptionBreakpointStopsAtThrow`, and re-run both the in-process C++ suite
   and the plugin integration suite against a freshly rebuilt `cajeta.exe`.

## Affected / relevant files

- `runtime/native/cajeta_runtime.c` — `__cajeta_throw`, `__cajeta_exc_push/pop`,
  `__cajeta_exc_top_ptr`, `__cajeta_trace_record`, the exception-handler hook.
- `src/cajeta/jit/CajetaJitHost.cpp` — `exceptionTrampoline`,
  `installExceptionHandler`, `startDebugSession` (arming + trace-capture-off).
- `src/cajeta/dap/DapServer.cpp` — `runToStopOrExit`, `setExceptionBreakpoints`,
  `configurationDone`.
- `ide-plugins/idea/src/test/kotlin/dev/cajeta/idea/debugger/CajetaDebugSessionIntegrationTest.kt`
  — the `@Ignore`'d `exceptionBreakpointStopsAtThrow` (un-ignore when fixed).

## Provenance

Diagnosed during CP6f-3 (exception breakpoints) on branch `idea-ide`. The
feature itself (server + runtime + plugin wire) is complete and committed
(CP6f-3a `e259716`, CP6f-3b `e13cc43`, CP6f-3c `04b2c26`); only the subprocess
throw path is blocked by this pre-existing defect.
