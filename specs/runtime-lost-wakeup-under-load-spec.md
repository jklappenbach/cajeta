# runtime-lost-wakeup-under-load — defect

## 1. Definition

A cajeta program whose fibers park and wake through the task scheduler can
wedge permanently: a parked fiber's wakeup is lost, the task never completes,
and every awaiter blocks forever. Observed 2026-07-31 in the rewritten
cajeta-http tour (10 sequential loopback client/server demos in one process).

**Severity corrected 2026-07-31 (later the same day).** The first
characterization here blamed CPU contention, because the initial ~30-50% hit
rate coincided with a heavy GPU/CPU sweep on the same box, and some runs
passed. A re-measurement on an IDLE machine (load 1.5 on 32 cores, no stray
processes, no socket pressure, zero TIME_WAIT) wedged **6 runs out of 6**, and
the same 6/6 reproduces with binaries built by BOTH cajeta 0.12.0 and 0.13.0 —
so this is neither load-gated nor a v0.13.0 regression. Load may widen the
window; it is not the cause. Treat this as a frequent, reliably reproducible
hang, not an occasional flake.

- 1.1 **Signature (gdb, all threads, wedged process).** Main thread blocked in
  `__futex_abstimed_wait` on `__cajeta_task_done_cond` (an `await` that never
  completes). All four worker threads idle on `__cajeta_task_queue_cond` — the
  run queue is EMPTY. The timer thread waits on `__cajeta_timer_cond` with
  `abstime = NULL` — NO pending timers. The reactor thread is parked in
  `epoll_wait`. So: a fiber that should be runnable (or should hold a pending
  timer) is neither queued nor timed — its wakeup was lost.
- 1.2 **Where it fires.** Every observed wedge sat at an HttpServer teardown:
  the tour's `srv.shutdown(deadline)` (whose drain polls with 100µs→10ms
  backoff sleeps) followed by `await` of the serve fiber. Passing a ZERO drain
  deadline (no backoff sleeps at all) did NOT cure it — the wedge moved to the
  same neighborhood — so the loss is not specific to the drain-poll's timers;
  it is a general park/wake race (task-done signaling or timer arming) that
  the teardown's burst of fiber transitions makes likely.
- 1.3 **Repro.** `cajeta-http` @ the unit-5 tour rewrite:
  `CAJETA_BIN=<cajeta> RUN=0 ./samples/tour/run.sh`, then loop
  `./samples/tour/build/http-tour`. Measured 2026-07-31 on an idle 32-core
  box: **6/6 wedged** (3 with a 0.13.0-built binary, 3 with 0.12.0), each
  hanging >120s against a normal ~15s runtime. Every wedge stopped at a
  `srv.shutdown(deadline)` + `await serveFiber` teardown — ServerDemo's or
  BodiesDemo's — never mid-request.
- 1.3a **Why CI is green.** The same tour passes in GitHub Actions (two runs,
  cajeta-http e500802 and its follow-up). Whatever the race needs, the runner
  does not provide it — so CI is NOT evidence the bug is rare, and a green CI
  badge should not be read as one.
- 1.4 **Impact.** Serious. Any program that shuts a fiber-based server down
  and awaits its accept loop can hang forever on a quiet machine — that is
  the ordinary teardown path, not an exotic one. The cajeta-http tour cannot
  currently be run to completion locally (tour-quality plan 9.1.1 records
  this); only CI gets through it.
- 1.5 **Non-goals.** The http library's shutdown/drain logic itself appears
  correct (deadline-bounded poll; listener close wakes the accept fiber); this
  spec targets the runtime's scheduler/timer wakeup path.

## 2. Acceptance

- 2.1 A stress harness (fiber park/wake churn under synthetic CPU contention)
  reproduces the wedge, then runs clean after the fix.
- 2.2 The cajeta-http tour loops ≥100 runs hang-free under the same synthetic
  load.
- 2.3 The gdb signature (empty run queue + timerless timer thread + waiting
  awaiter) is impossible by construction: any parked fiber is always either
  queued, timed, or registered with the reactor.
