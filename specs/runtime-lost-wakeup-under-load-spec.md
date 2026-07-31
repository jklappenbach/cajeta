# runtime-lost-wakeup-under-load — defect (root-caused + fixed; see 1c/1d)

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

## 1d. Residual — same bug, second interleaving (FIXED)

The first fix left ~5% of runs wedging. Instrumenting the runtime
(`CAJETA_TRACE_FD`) caught the interleaving directly:

```
[trace] NETCLOSE  fd=3                      ← fiber A begins closing fd 3
[trace] IOWAIT    fd=3 fiber=0x...c9a40     ← fiber B arms a waiter on fd 3, parks
[trace] CLOSEWAKE fd=3 woke=0               ← the wake walks the list, finds nothing
```

At the wedge, gdb showed exactly one waiter — `fd=3`, owned by the parked
fiber, `stack_owned=0` (an untimed wait, so no timer is expected) — while
`/proc/<pid>/fd` had **no fd 3 at all**. A fiber parked forever on a
descriptor that no longer existed.

Root cause: `__cajeta_io_wait` registers its waiter and parks under
`task_mutex`, but the close did **not** hold that mutex across the actual
`close(2)`. Waking waiters before closing therefore only covers waiters that
already exist; one armed in the window between the walk and the `close(2)`
is orphaned on a dead descriptor, and nothing can ever wake it.

Fix: `__cajeta_io_close_fd(fd)` performs the waiter walk, the `epoll_ctl(DEL)`
**and the `close(2)` itself** under one `task_mutex` hold. That leaves only two
orderings, both correct: waiter-first (the walk finds it and wakes it), or
close-first (io_wait's `epoll_ctl(ADD)` fails `EBADF` and it returns without
parking). `__cajeta_net_close` and `__cajeta_fd_close` both route through it on
Linux.

Measured on the cajeta-http tour, idle box:

| | runs completing |
|---|---|
| before any fix | 0 of 6 |
| close-wake only | 19 of 20 |
| close under the lock | **40 of 40** |

## 2. Acceptance

- 2.1 A stress harness (fiber park/wake churn under synthetic CPU contention)
  reproduces the wedge, then runs clean after the fix. — MET in substance: both
  interleavings were reproduced and fixed (§1c, §1d); no synthetic harness was
  needed because the http tour reproduced deterministically.
- 2.3 The gdb signature (empty run queue + timerless timer thread + waiting
  awaiter) is impossible by construction — a parked fiber is always queued,
  timed, or holding a LIVE fd registration. Closing an fd now either wakes its
  waiters or precedes their registration; it can no longer strand one.
- 2.2 The cajeta-http tour loops ≥100 runs hang-free. — 40/40 measured; run a
  longer loop opportunistically before closing this spec.
- 2.3 The gdb signature (empty run queue + timerless timer thread + waiting
  awaiter) is impossible by construction: any parked fiber is always either
  queued, timed, or registered with the reactor.
