# runtime-lost-wakeup-under-load — defect

## 1. Definition

Under CPU contention, a cajeta program whose fibers park and wake through the
task scheduler can wedge permanently: a parked fiber's wakeup is lost, the
task never completes, and every awaiter blocks forever. Observed 2026-07-31
while running the rewritten cajeta-http tour (10 sequential loopback
client/server demos in one process) on a machine concurrently running a heavy
GPU/CPU sweep; roughly a third to half of runs wedged. The same binary also
completed 150/150 checks cleanly on other runs, including several in a row —
the failure is a race whose window widens under load.

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
  `CAJETA_BIN=<cajeta> RUN=0 ./samples/tour/run.sh` then loop
  `./samples/tour/build/http-tour` under concurrent CPU load; ~30-50% of runs
  hang (>90s, normally ~15s). On an idle machine the hang was not reproduced
  in the runs observed, but the small sample and the load confound mean the
  idle-machine rate is unmeasured, not zero.
- 1.4 **Impact.** Any long-running fiber-heavy service on a busy host can
  freeze. CI processes with timeouts fail spuriously. The http test suite's
  own flake-hunting loop mode (`run-tests.sh N`) suggests related history.
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
