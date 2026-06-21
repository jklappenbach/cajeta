---
id: time-instant-duration
applies-to: [cajeta/time/Instant, cajeta/time/Duration]
title: Instant + Duration — the machine timeline (points and spans)
description: Pair Instant (a UTC point) with Duration (an elapsed span); between/plus/minus wire them, and Duration wraps Clock.nanoTime() deltas.
---

# Instant + Duration — the machine timeline

Use this pair for **machine time**: timestamps and elapsed amounts, not calendars.
`Instant` is a *point* on the UTC timeline; `Duration` is a *signed span* between two
points. They are the two halves of one arithmetic:

- `Instant.between(start, end)` → `Duration` (point − point = span)
- `instant.plus(d)` / `instant.minus(d)` → `Instant` (point ± span = point)
- `Duration.plus/minus/multipliedBy/dividedBy/negated/abs` → `Duration` (span algebra)

For calendar breakdown (year/month/day, zones, formatting) this is the wrong component —
go to `cajeta/time/LocalDateTime`, `ZonedDateTime`, `ZoneOffset`, `Period`. Project an
`Instant` to a calendar via `ZonedDateTime.ofInstant(instant, offset)`.

## Members and roles

- **`Instant`** — an immutable moment, `int64 epochSecond` (since 1970-01-01T00:00:00Z,
  may be negative) + `int32 nano` (0..999999999, always normalized). Implements
  `Comparable<Instant>`.
- **`Duration`** — an immutable, **signed** time amount, one `int64` nanosecond count
  (range ≈ ±292 years). Negative is valid and meaningful. Implements
  `Comparable<Duration>`.

## Ownership & lifecycle (read this once, applies to both)

Both are **pure `stack` value types passed and returned by copy**. Consequences:

- **No `#` transfer anywhere** — no factory, transform, or `between`/`plus`/`minus` takes
  or yields ownership; arguments are borrowed, results are fresh `stack` values.
- **No `close()`, no dispose, no drop hook, no heap state.** Nothing to free; storing one
  in a field or returning it is just a copy.
- **Immutable.** Every operation returns a *new* value; the receiver is never mutated.
  `t.plus(d)` does nothing unless you bind its result.

## Collaboration / call sequence

The canonical machine-time flow — measure an interval and stamp it:

```cajeta
import cajeta.time.Instant;
import cajeta.time.Duration;
import cajeta.time.Clock;

stack Instant start = Clock.now();          // a point (wall clock, ms resolution)
doWork();
stack Instant end   = Clock.now();          // a later point

stack Duration elapsed = Instant.between(start, end);  // point − point = span
int64 ms = elapsed.toMillis();              // 0 if work was sub-millisecond

stack Instant deadline = start.plus(Duration.ofSeconds(30));  // point + span
boolean late = end.isAfter(deadline);
```

For sub-millisecond / monotonic timing, **don't** use `Instant` (it is wall-clock and
only ms-resolution via `Clock.now()`). Build the `Duration` straight from
`Clock.nanoTime()` deltas — that is exactly the type to wrap a nano delta:

```cajeta
import cajeta.time.Duration;
import cajeta.time.Clock;

stack int64 t0 = Clock.nanoTime();          // monotonic counter, not wall time
doWork();
stack Duration took = Duration.ofNanos(Clock.nanoTime() - t0);
```

## When to use which

- Need a *moment* (when something happened, a deadline, a timestamp to compare or sort):
  `Instant`. Equality/order: `==`, `compareTo`, `isBefore`, `isAfter`.
- Need an *amount* (how long, a timeout, an interval, the result of `nanoTime()` math):
  `Duration`. Build with `ofNanos/ofMillis/ofSeconds/ofMinutes/ofHours/ofDays` or
  `zero()`; read back with the `toX()` accessors (all truncate toward zero).

## Sharp edges

- `Duration` is **signed**: `Instant.between(a, b)` is negative when `b` precedes `a`;
  check with `isNegative()` / normalize with `abs()` if you need magnitude.
- `Duration.toX()` accessors **truncate**: `Duration.ofMillis(90500).toMinutes()` is `1`.
- Day/hour conversions are fixed-length (1 day = 86400 s, no calendar/DST awareness). For
  calendar-aware spans use `Period`.
- `Clock.nanoTime()` is monotonic and **not** wall-clock — never feed it to
  `Instant.ofEpoch*`; only use its *deltas*, wrapped in a `Duration`.
- Binary value-returning operators are intentionally absent (the borrow-return rule): use
  the named methods `plus`/`minus`, not `+`/`-`, on these types. Only `==` (returns a
  primitive) is overloaded.
