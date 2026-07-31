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

## 1b. Root cause (found 2026-07-31)

**Closing a descriptor never wakes the fibers parked on it.**

`__cajeta_io_wait` (runtime/native/cajeta_rt_concurrent_exec.c) registers a
waiter, arms `epoll_ctl(ADD, fd, …|EPOLLONESHOT)`, and parks the fiber on
`__cajeta_parked_head`. The only thing that republishes it is the reactor
thread matching an epoll event for that fd.

`__cajeta_net_close` (runtime/native/cajeta_net_socket.c) was a bare
`close(fd)`. On Linux, closing the last reference to an open file description
removes it from every epoll interest list **silently** — epoll(7) Q6/A6 — so
no event is ever delivered for it. The parked fiber is therefore never
published: it stays on `parked_head` forever, the pool drains empty, no timer
is armed, and every awaiter of that fiber's task blocks permanently. That is
exactly the captured signature.

The failure is deterministic, which is why it presented at server teardown:
`Server.shutdown()` closes the listener while the accept fiber is parked on
listener-readability, then joins that fiber. `cajeta.io.net.Server.serve()`
is written against the opposite promise — its own doc says *"when shutdown
closes the listener, the parked acceptAsync unblocks with a NetException"* —
so the library was relying on a runtime guarantee that did not exist.

**Why it ever appeared to work, and why it looked load-dependent.** The
reactor matches waiters by **fd number**:
`if ((*p)->fd == fd) { … publish … }`. Once the listener's fd is closed its
number is free for reuse; if a later socket happened to be assigned that same
number and then produced an event, the stale waiter matched and the accept
fiber was published *by accident*. Runs with heavy connection churn recycled
descriptors often enough to unwedge themselves; a quiet teardown never did.
Load did not widen a race — it supplied the accidental rescue. (That aliasing
is a latent correctness bug in its own right: a recycled descriptor could
deliver a wakeup to a fiber that never asked for it.)

## 1c. Fix

`__cajeta_io_close_wake(int32_t fd)`, called from `__cajeta_net_close` (and
`__cajeta_fd_close`) **before** the `close(2)`. Under `task_mutex` it drops
the epoll registration while the descriptor is still valid, detaches every
waiter for that fd, and publishes their fibers with the same protocol the
reactor's ready path uses (`fired = 1`; free only heap-owned waiters). A woken
fiber retries its accept/read, gets `EBADF`, and the library maps that to the
`NetException` its accept loop already expects. Detaching the waiters also
closes the fd-number aliasing window.

Measured on the cajeta-http tour, idle box, same binary shape as the repro:

| | before | after |
|---|---|---|
| runs completing | 0 of 6 | 19 of 20 |

## 1d. Residual (separate bug, still open)

~5% of runs still wedge, also at a teardown. This is NOT the close path: the
carrier sleep uses `pthread_cond_timedwait` with a 50 ms backstop, so a missed
*deque* signal self-heals, which means the residual fiber is parked with **no
wakeup source armed at all** (no timer in the timer thread, no live fd
registration). The likely shape is a server connection fiber parked on an
untimed read against a client socket that is never closed — the peer is an
in-process fiber whose pooled connection is dropped rather than closed, so no
EOF is ever delivered and no deadline exists to rescue it. Needs its own
investigation; the 20x improvement above should not be read as a full fix.

## 2. Acceptance

- 2.1 A stress harness (fiber park/wake churn under synthetic CPU contention)
  reproduces the wedge, then runs clean after the fix. — PARTIAL: the close
  path is fixed and measured (§1c); no synthetic harness written yet.
- 2.2 The cajeta-http tour loops ≥100 runs hang-free. — NOT MET: 19/20 after
  the close fix; the §1d residual still wedges ~5% of runs.
- 2.3 The gdb signature (empty run queue + timerless timer thread + waiting
  awaiter) is impossible by construction: any parked fiber is always either
  queued, timed, or registered with the reactor.
