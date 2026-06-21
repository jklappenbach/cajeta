---
id: time-Clock
applies-to: [cajeta/time/Clock]
title: Clock — the static "what time is it" entry point
description: Pick nanoTime (monotonic intervals) vs millisTime (wall millis) vs now() (Instant); all static, nothing to construct.
---

# Clock

The static entry point for reading the current time in `cajeta.time`. It is an
**access point, not a value type**: every member is `static`, the class is
`final`, and there is **nothing to construct** — never write `new Clock()` or
`stack Clock(...)`. Call straight through the type name: `Clock.nanoTime()`.

## Which one do I call?

| Want | Call | Returns |
| --- | --- | --- |
| Measure an elapsed interval / benchmark | `Clock.nanoTime()` | `int64` nanoseconds, monotonic counter |
| Wall-clock milliseconds since the Unix epoch | `Clock.millisTime()` | `int64` ms (CLOCK_REALTIME) |
| The current moment as a calendar-projectable value | `Clock.now()` | `stack Instant` (millisecond resolution) |

`nanoTime()` reads `CLOCK_MONOTONIC`; `millisTime()` reads `CLOCK_REALTIME`.
Both are `@Native`-bound to the runtime. `now()` is plain cajeta built on
`millisTime()`.

## Signatures

```cajeta
public static int64 nanoTime();     // CLOCK_MONOTONIC, nanoseconds
public static int64 millisTime();   // CLOCK_REALTIME, milliseconds since epoch
public static Instant now();        // wall clock as an Instant
```

`now()` returns a fresh `stack Instant` by copy — a pure value, nothing to free
and no aliasing. The two `int64` results are primitives. No member returns null
and none raise.

## Example (with imports)

```cajeta
import cajeta.time.Clock;
import cajeta.time.Instant;
import cajeta.time.Duration;
import cajeta.time.ZonedDateTime;
import cajeta.time.ZoneOffset;

// Time an operation with the monotonic counter — the right tool for durations.
stack int64 start = Clock.nanoTime();
doWork();
stack Duration elapsed = Duration.ofNanos(Clock.nanoTime() - start);

// Capture the current moment and project it onto a calendar.
stack Instant moment = Clock.now();
stack ZonedDateTime here = ZonedDateTime.ofInstant(moment, ZoneOffset.utc());
```

## What it does NOT do — avoid these dead ends

- **`nanoTime()` is not wall-clock time.** It is an arbitrary-origin monotonic
  counter for measuring *deltas* only. Do not treat it as epoch nanos, do not
  feed it to `Instant`, and do not compare counters across processes. It is
  unaffected by NTP/leap-second/clock adjustments — which is exactly why
  `millisTime()` (and `now()`) CAN jump backward or forward and `nanoTime()`
  cannot.
- **There is no calendar/timezone API on `Clock`.** It only reads raw time. For
  fields (year/hour/zone) project an `Instant` through `ZonedDateTime` /
  `LocalDateTime` at a `ZoneOffset` — see `cajeta/time/Instant`,
  `cajeta/time/ZonedDateTime`, `cajeta/time/ZoneOffset`.
- **`now()` is millisecond-resolution**, not nanosecond — its `Instant` always
  has `nano % 1000000 == 0`. Use `nanoTime()` deltas when you need finer timing.
- No injectable/fixed clock: `Clock` always reads the real system clock; there
  is no `Clock.fixed(...)` for testing.
