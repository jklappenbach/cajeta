---
id: time-calendar
applies-to: [cajeta/time/LocalDate, cajeta/time/LocalTime, cajeta/time/LocalDateTime]
title: Zone-free calendar value types (LocalDate / LocalTime / LocalDateTime)
description: How LocalDate, LocalTime, and LocalDateTime compose, decompose, and cross to the machine timeline via a ZoneOffset.
---

# Zone-free calendar values

Three immutable **stack value types** for wall-clock dates and times with **no
time zone**. Reach here when you need to hold or compute a calendar date, a
time-of-day, or both, and you do *not* yet care about a zone or an absolute
instant.

- **`LocalDate`** — a calendar date (`yyyy-MM-dd`), one `int64` epoch-day.
- **`LocalTime`** — a time-of-day (`HH:mm:ss.nnnnnnnnn`), one `int64`
  nanosecond-of-day in `[0, 86399999999999]`.
- **`LocalDateTime`** — a `LocalDate` + `LocalTime` together
  (`int64 epochDay` + `int64 nanoOfDay`).

Each carries fuller detail in its own class skill; this skill is about how they
**cooperate**.

## Object graph: compose and decompose

`LocalDateTime` *is* the composition. It does not reference a `LocalDate`/`LocalTime`
instance — it stores their two raw fields inline.

- **Compose:** `LocalDateTime.ofDateTime(date, time)` reads `date.toEpochDay()`
  and `time.toNanoOfDay()` into a fresh `LocalDateTime`.
- **Decompose:** `dt.toLocalDate()` and `dt.toLocalTime()` each rebuild a fresh
  value from the stored field.

Round-trip is exact: `LocalDateTime.ofDateTime(dt.toLocalDate(),
dt.toLocalTime())` equals `dt`. You can also build a `LocalDateTime` directly with
`of(y,mo,d,h,mi)` or `ofFull(y,mo,d,h,mi,s,nano)` and skip the parts entirely.

## Ownership & lifecycle

- All three are **value types returned by `stack` copy**. Every `of*`/`plus*`/
  `to*` returns a *fresh* value; the receiver is never mutated. Pass and return
  them by value — there is **no `#` transfer**, no `close()`, no heap handle to
  free. `ofDateTime` takes its `LocalDate`/`LocalTime` arguments by value and
  copies their fields; the arguments remain valid afterward.
- The **one boundary that transfers ownership** is `iso()`: each type returns an
  owned `#String` (a freshly heap-allocated `String`) that the caller owns and
  must release. See `cajeta/lang/String`.

## Errors

Out-of-range or non-existent fields throw `cajeta/time/DateTimeException` (e.g.
month not in `1..12`, February 30, hour `> 23`). This is raised by the validating
factories (`LocalDate.of`, `LocalTime.of`/`ofHms`/`ofHmsn`, `LocalDateTime.of`/
`ofFull`) and by `ZoneOffset` factories. The raw constructors
(`LocalDate(epochDay)`, `LocalTime(nanoOfDay)`, `LocalDateTime(epochDay,
nanoOfDay)`) and the epoch/nano factories do **not** validate — use them only
when you already hold canonical counts.

## Arithmetic carries differently — pick the right type

This is the main correctness trap when choosing among the three:

- **`LocalTime.plusHours/plusMinutes/plusSeconds/plusNanos` wrap within a single
  day.** `LocalTime.of(23,30).plusHours(1)` is `00:30` — the day overflow is
  *discarded*. There is no date to carry into.
- **`LocalDateTime.plus*` carries the overflow into the date.**
  `LocalDateTime.of(2026,6,5,23,30).plusHours(1)` rolls to `2026-06-06T00:30`.
- **`LocalDate.plusMonths/plusYears` clamp** the day to the target month
  (2026-01-31 + 1 month = 2026-02-28); `plusDays`/`plusWeeks` are exact.

So: if adding time must be allowed to change the date, use `LocalDateTime`, not
`LocalTime`.

## Crossing to the machine timeline needs a ZoneOffset

These types are zone-free, so they **cannot** be turned into an absolute
`Instant` on their own. Only `LocalDateTime` crosses over, and only when given a
`cajeta/time/ZoneOffset`:

- `dt.toEpochSecond(offset)` → `int64` seconds since 1970.
- `dt.toInstant(offset)` → a `cajeta/time/Instant` (also a stack value).

`LocalDate`/`LocalTime` have no such method — compose a `LocalDateTime` first.

## What these do NOT do

- **No time zone, no DST, no "now."** There is no `now()` / current-time factory
  here; for the current instant use `cajeta/time/Clock` / `Instant`, and for
  zone rules use `cajeta/time/ZonedDateTime` + `ZoneId`.
- **No `toString` override** — format with `iso()` (or `cajeta/time/
  DateTimeFormatter`).
- **No tuple return** — `getYear()`/`getMonthValue()`/`getDayOfMonth()` are
  separate accessors.

## When to use which

Date only → `LocalDate`. Clock time only → `LocalTime`. Both, or any arithmetic
that must roll the date → `LocalDateTime`. Need an absolute point on the
timeline → cross via `LocalDateTime.toInstant(offset)`.

## Worked example (compose, decompose, cross over)

```cajeta
import cajeta.time.LocalDate;
import cajeta.time.LocalTime;
import cajeta.time.LocalDateTime;
import cajeta.time.ZoneOffset;
import cajeta.time.Instant;
import cajeta.lang.String;

// Compose a date + a time into a zone-free date-time.
stack LocalDate day = LocalDate.of(2026, 6, 5);          // throws DateTimeException if invalid
stack LocalTime tod = LocalTime.of(23, 30);              // 23:30
stack LocalDateTime dt = LocalDateTime.ofDateTime(day, tod);

// Arithmetic on the date-time carries the overflow into the date.
stack LocalDateTime next = dt.plusHours(1);              // 2026-06-06T00:30
stack LocalDate rolled = next.toLocalDate();             // 2026-06-06 (decompose)

// Cross into the machine timeline by supplying a zone offset.
stack Instant at = next.toInstant(ZoneOffset.utc());

String text #= next.iso();                               // owned "2026-06-06T00:30"; caller frees
```

Note the contrast: `tod.plusHours(1)` alone would wrap to `00:30` on the *same*
day and lose the rollover — that is why the rollover example operates on
`LocalDateTime`.
