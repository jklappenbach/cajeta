# localdate-loop-reassign — `d = d.plusDays(1)` in a loop produces the wrong date (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 2 while
> re-testing the first suite's skip reasons.

## 1. Definition
Measured on v0.46.3 in both `--debug` and `--release`: starting from
`LocalDate.of(2000, 1, 1)`, a loop of 366 iterations of `d = d.plusDays(1)`
ends at **1970-01-02**. The straight-line form of the same reassignments and
the single call `of(2000,1,1).plusDays(366)` are both correct. `plusDays`
returns `stack LocalDate(...)`. The same loop over `Instant` is correct, so
the first suite's "value-type in-place reassignment in a loop is a no-op" is
half fixed: `Instant` works, `LocalDate` does not. Both time benchmarks carry
a comment naming this spec and derive a fresh date per iteration until it is
fixed.

**Broader than LocalDate** (unit 8, 2026-10-25): the same 366-step
reassignment loop is wrong for `Duration.plus` and `LocalDateTime.plusHours`
as well; `LocalTime.plusSeconds` and `Instant` are correct. Every new time
and hash benchmark builds a fresh value per step and names this spec in its
notes.

## 2. Use cases
- **2.1** As a developer, when I reassign a stack value-type local from a
  method call on itself inside a loop, then the local holds the returned
  value on every iteration.
- **2.2** As the test suite, then a JIT test runs the 366-day loop and
  asserts 2001-01-01, and a second test does the same for a user-defined
  stack value type with a method returning `stack T(...)`.

## 3. Acceptance
`time/time-localdate-arith` can be rewritten as the loop and still check
`ok`; the workaround comments are removed.
