# command-run-stdin-pipe-deadlock — `Command.run()` with `stdinBytes` deadlocks past one pipe buffer (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 13 on
> v0.46.3, measured with 1 MiB through `cat`.

## 1. Definition
`Command.run()` given `stdinBytes` writes all of stdin before it drains
stdout. Once the input exceeds one pipe buffer the child blocks on its
stdout and the parent blocks on its stdin, and the run's timeout cannot
fire. `process/process-pipe-100mb` works around it with `Command.start` and
alternating 64 KiB writes and reads. Related gaps from the same unit:
`CompressStream.write`, `FileWriter.write` and `FileReader.read` take no
offset, so every piece is copied first; `Schema` is opaque to callers.

## 2. Use cases
- **2.1** As a caller of `Command.run()` with input larger than a pipe buffer,
  then stdin is written while stdout and stderr are drained (a reactor-driven
  pump or a writer fiber), and the timeout fires if the child stalls.
- **2.2** As the test suite, then a test pipes 8 MiB through `cat` with
  `run()` and asserts the output and a bounded wall time; a second test
  gives a child that never reads and asserts the timeout fires.

## 3. Acceptance
`process/process-pipe-100mb` uses `Command.run()` and its workaround note is
removed.
