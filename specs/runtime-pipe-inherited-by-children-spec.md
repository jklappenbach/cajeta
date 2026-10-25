# runtime-pipe-inherited-by-children — the runtime's pipe lacks close-on-exec (defect spec)

> Status: **draft**, filed 2026-10-25 by the dev.cajeta.bench author on
> v0.46.3.

## 1. Definition
The runtime holds a pipe (both ends) without `O_CLOEXEC`, so every child
process a program spawns inherits both descriptors. Harmless for the
benchmarks, but a child that outlives the parent keeps the pipe open, and a
child that closes or writes inherited descriptors can interfere with the
runtime. Related observation from the same work: the socket `connectAsync`
returns is non-blocking, so a plain `read` yields the would-block sentinel;
`readAsync` is required, which the docs should say.

## 2. Use cases
- **2.1** As a program spawning children, then no runtime descriptor is
  inherited: the pipe is created with `O_CLOEXEC` (or `pipe2`), and the same
  for any other runtime-owned descriptor (the reactor's epoll and event fds).
- **2.2** As the test suite, then a test spawns `ls -l /proc/self/fd` through
  `Command` and asserts no descriptor beyond 0, 1, 2 is present.

## 3. Acceptance
The test passes on Linux; the `connectAsync` doc states the socket is
non-blocking.
