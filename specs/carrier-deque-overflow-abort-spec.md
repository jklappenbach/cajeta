# carrier-deque-overflow-abort — a burst of spawns or a close with many waiters aborts the process (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 7 while
> building `concurrent/tasks-1m-footprint` on v0.46.3.

## 1. Definition
The carrier run deque is a fixed 2048 slots and aborts on overflow. Two
ordinary shapes hit it: a burst of more than about 2048 `spawn`s from the
main thread, and `Channel.close()` with more than about 2048 fiber waiters.
The benchmark paces spawns in steps of 1024 and parks tasks on 1000 channels
of 1000 waiters each to stay under it. Two related measurements from the
same work: every fiber is two mappings (a guard page and its stack), so
`vm.max_map_count` of 1,048,576 caps parked fibers near 520k and a 1M probe
hung at the map limit instead of aborting; and spawn-plus-await measures
150 to 210 µs on Phoenix against the 24.6 µs the old `TaskSpawnBench` comment
recorded, so `concurrent/task-spawn-await` has regressed by about 7×
(`task-spawn-await-regression`, same filing).

## 2. Use cases
- **2.1** As a program, when I spawn 1e5 tasks in a loop, then the runtime
  queues or grows rather than aborting; a bounded deque spills to an
  overflow list or blocks the spawner.
- **2.2** As a program, when I close a channel with 1e5 waiters, then every
  waiter wakes with the closed result and nothing aborts.
- **2.3** As the runtime, when a fiber is parked, then its guard page does
  not cost a second mapping, or the limit is documented and checked at spawn
  with a diagnostic naming `vm.max_map_count`.
- **2.4** As the test suite, then a test spawns 1e4 tasks in a burst and a
  test closes a channel with 1e4 waiters; and `concurrent/task-spawn-await`
  returns to its recorded 25 µs class, with the cause of the regression
  named.

## 3. Acceptance
`tasks-1m-footprint` runs unpaced on a single channel at 1e6 on Phoenix
without a sysctl change; `task-spawn-await` is within 2× of its 2026-06
figure.
