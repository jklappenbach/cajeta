# file-openread-missing-returns-reader — `File.openRead` on a missing path returns a reader instead of null (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 12 (also
> seen by the dev.cajeta.bench author) on v0.46.3.

## 1. Definition
`File.openRead(path)` on a path that does not exist returns a reader whose
`fd` is -1 rather than null, and the documented null check never fires;
`readAllBytes` on that reader returns null. A missing dataset read as present
and the benchmark crashed later. There is also no `File.exists`.

## 2. Use cases
- **2.1** As a caller, when the file is missing, then `openRead` returns null
  (or throws, per the io doctrine), never a dead reader; and `File.exists`
  exists.
- **2.2** As the test suite, then a test opens a missing path and asserts the
  documented result; a test on a dead reader is deleted when it can no longer
  be produced.

## 3. Acceptance
The nucleo and codec benchmarks' `readAllBytes` null checks revert to the
documented `openRead` check.
