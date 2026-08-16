---
id: time-overview
applies-to: [cajeta.time]
title: cajeta.time — date/time orientation and task routing
description: Pick the right cajeta.time type (Instant/Duration, Local*, Zoned*, DateTimeFormatter) and learn its value-vs-reference ownership and error model.
---

# cajeta.time — orientation & routing

A `java.time`-style date/time library. The core split: a **machine timeline**
(`Instant` + `Duration`, UTC nanoseconds) versus **human calendar values**
(`LocalDate`/`LocalTime`/`LocalDateTime`, `Period`) and **zones**
(`ZoneOffset`/`ZoneId` → `ZonedDateTime`). Text comes out via
`DateTimeFormatter`. If your task is "what time is it, how long was that, what
day is it, render it" — you are in the right place.

## Task → entry point

| I want to… | Start with |
| --- | --- |
| Get the current moment | `Clock.now()` → `Instant` |
| Measure an elapsed interval (benchmark) | `Clock.nanoTime()` deltas → `Duration.ofNanos(...)` |
| A point on the UTC timeline | `Instant.ofEpochSecond/ofEpochMilli/epoch` |
| A fixed time amount (seconds…days) | `Duration.ofSeconds/ofMinutes/ofHours/…` |
| Elapsed time between two instants | `Instant.between(start, end)` → `Duration` |
| A calendar date (no time, no zone) | `LocalDate.of(y, m, d)` |
| A time-of-day (no date, no zone) | `LocalTime.of(h, m)` / `ofHms` / `ofHmsn` |
| Date + time, no zone | `LocalDateTime.of(...)` / `ofFull` / `ofDateTime` |
| A calendar amount (years/months/days) | `Period.of(y, m, d)` |
| A fixed UTC offset (`+05:30`, `Z`) | `ZoneOffset.ofHours/ofHoursMinutes/utc` |
| A DST-aware region zone (`Europe/Paris`) | `ZoneId.of("…")` then `.offsetAt`/`.resolve` |
| A moment with an offset attached | `ZonedDateTime.ofInstant(i, off)` / `ofLocal(ldt, off)` |
| ISO-8601 text fast | the value's own `iso()` method |
| strftime / standard-style text | `DateTimeFormatter.ofPattern("%Y-%m-%d")` / `ofStandard(FormatStyle.…)` |
| **Parse text into a value** | **Not provided** — see Hazards. There is no parser. |
| **Add months/years to an Instant** | **Not directly** — project to `LocalDate`/`LocalDateTime`, apply a `Period`, convert back. `Instant`/`Duration` only do fixed nanos. |
| **DST-aware arithmetic over a span** | **Not provided** — `ZonedDateTime.plus(Duration)` keeps the *same* fixed offset; re-resolve through `ZoneId` if a transition may be crossed. |

## Cross-cutting invariants

- **Value types are stack values returned by copy** — `Instant`, `Duration`,
  `LocalDate`, `LocalTime`, `LocalDateTime`, `ZonedDateTime`, `ZoneOffset`,
  `Period`. Every factory and transform returns a fresh `stack` value; they are
  immutable, hold no heap state, and you neither own nor free them. Bind them to
  `stack` locals.
- **Reference types are heap objects whose ownership transfers to you (`#`).**
  `ZoneId.of(...)` returns `#ZoneId`; `DateTimeFormatter.ofPattern/ofStandard`
  return `#DateTimeFormatter`; `DateTimeFields.of*` return `#DateTimeFields`.
  These are `#`-owned — you are responsible for them.
- **Every `iso()` and every `DateTimeFormatter.format*` returns an owned
  `#String`.** Ownership transfers to the caller; the result is a fresh buffer,
  not a borrowed view.
- **Errors are recoverable exceptions, not sentinels or optionals.**
  `DateTimeException extends RecoverableException`, so `catch` handlers see it
  (the runtime does not abort). Validating factories throw it on out-of-range
  fields: `LocalDate.of(2026, 13, 1)`, `LocalTime.of(24, 0)`,
  `ZoneOffset.ofHours(20)`, and `ZoneId.offsetAt` on an unknown zone all throw.
  Non-validating `ofEpoch*`/raw constructors do **not** validate.
  `DateTimeParseException extends DateTimeException` (adds `errorIndex`).
- **Day-of-week is ISO: Monday = 1 … Sunday = 7.** Months are 1–12.
- No nullable returns in this package; absence is signalled by throwing.

## Canonical end-to-end

```cajeta
import cajeta.time.Clock;
import cajeta.time.Instant;
import cajeta.time.ZoneOffset;
import cajeta.time.ZonedDateTime;
import cajeta.time.DateTimeFormatter;
import cajeta.time.FormatStyle;
import cajeta.lang.String;

stack Instant now = Clock.now();                       // wall-clock moment
stack ZoneOffset off = ZoneOffset.ofHours(-5);         // throws if |hours| > 18
stack ZonedDateTime zdt = ZonedDateTime.ofInstant(now, off);
int32 wallHour = zdt.getHour();                        // hour at -05:00

DateTimeFormatter fmt #=
    DateTimeFormatter.ofStandard(FormatStyle.ISO_OFFSET_DATE_TIME);
String text #= fmt.formatZoned(zdt);   // owned; e.g. "2026-06-20T08:30:00-05:00"
```

For a one-off ISO string, skip the formatter entirely: `zdt.iso()` returns the
same owned `#String`.

## Disambiguation

- **`Duration` vs `Period`.** `Duration` is a *fixed* count of nanoseconds
  (±292 years); it does machine-time math on `Instant`/`ZonedDateTime` and never
  knows about calendars (`Duration.ofDays(1)` is exactly 86400 s). `Period` is a
  *calendar* amount of years/months/days where a month is not a fixed length;
  use it only with `LocalDate.plus(Period)` / `LocalDateTime`.
- **`Clock.now()` vs `Clock.nanoTime()`.** `now()`/`millisTime()` read the
  wall clock (`CLOCK_REALTIME`) — real dates, but they can jump with NTP/leap
  adjustments. `nanoTime()` is a monotonic counter (`CLOCK_MONOTONIC`) — NOT a
  date; use it (and only it) for measuring elapsed intervals.
- **`ZoneOffset` vs `ZoneId`.** `ZoneOffset` is one fixed number (a value type);
  `ZoneId` is a DST-aware region that yields *different* offsets at different
  instants (a reference type backed by the tz database). `ZonedDateTime` itself
  stores only a fixed offset, so to honour DST across a span re-resolve via
  `ZoneId.resolve`/`offsetAt`.

## Hazards

- **There is no general string parser.** `DateTimeFormatter` only *formats*.
  `DateTimeParseException` exists and some docstrings show `LocalDate.parse(...)`
  / `LocalTime.parse(...)`, but **no such method exists** in the source — do not
  reach for them. Build values from integer fields, not from text.
- **The UTC factory is `ZoneOffset.utc()` (lowercase).** A couple of docstrings
  write `ZoneOffset.UTC()`; that is a typo, not an API.
- **Non-validating constructors trust you.** The raw `Instant(s, n)`,
  `LocalDate(epochDay)`, `LocalTime(nanoOfDay)`, `ZoneOffset(seconds)`, etc., do
  no range checks. Prefer the `of*` factories; only use the constructor when you
  already hold a canonical field.
- **`ZoneId` needs the system tz database** (`/usr/share/zoneinfo`, POSIX) for
  non-UTC zones; `UTC`/`GMT`/`Z`/`Etc/UTC` resolve without touching the
  filesystem, so they work in static builds. Unknown/unavailable zones throw from
  `offsetAt`/`resolve`, not from `ZoneId.of`.
- **Calendar arithmetic clamps, it does not roll.** `LocalDate.plusMonths`/
  `plusYears` clamp the day to the target month (Jan 31 + 1 month → Feb 28).
- **No tuple returns:** year/month/day come from separate accessors
  (`getYear()`, `getMonthValue()`, `getDayOfMonth()`), each recomputing.

## Setup

Library `cajeta.time`; import per type, e.g. `import cajeta.time.Instant;`,
`import cajeta.time.LocalDate;`. `#String` results need `import cajeta.lang.String;`.
`Clock.nanoTime`/`millisTime` and `ZoneId` lookups are POSIX/Linux native calls.

## Going deeper

Per-type detail (signatures, fields, lifecycle) lives in each class's own
source/skill: `Instant`, `Duration`, `LocalDate`, `LocalTime`, `LocalDateTime`,
`ZonedDateTime`, `ZoneOffset`, `ZoneId`, `Period`, `DateTimeFormatter`,
`FormatStyle`, `DateTimeFields`, `DateTimeException`, `DateTimeParseException`,
`Clock`.
