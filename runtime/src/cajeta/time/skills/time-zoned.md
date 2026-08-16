---
id: time-zoned
applies-to: [cajeta/time/ZonedDateTime, cajeta/time/ZoneOffset, cajeta/time/ZoneId]
title: Zoned date-time conversion — fixed offset vs region id
description: Build a ZonedDateTime from an Instant or LocalDateTime; pick fixed ZoneOffset (value) vs region ZoneId (owned, DST-aware).
---

# Zoned date-time conversion

Three cooperating types turn a UTC `Instant` (or a wall-clock `LocalDateTime`) into a
`ZonedDateTime`. Pick your path by two questions:

1. **Do you already know the offset, or do you have a region name?**
   - Known fixed offset (`-05:00`, `Z`, `+05:30`) → build a `ZoneOffset` and call a
     `ZonedDateTime` factory directly.
   - Region name (`"America/New_York"`) whose offset depends on the date (DST) → make a
     `ZoneId` and let it derive the offset from the tz database.
2. **Which side do you have — the instant, or the wall-clock fields?**
   - Have an `Instant` (a point on the UTC timeline) → `ZonedDateTime.ofInstant` /
     `ZoneId.resolve`.
   - Have a `LocalDateTime` (wall-clock fields you assert are *at* that offset) →
     `ZonedDateTime.ofLocal`.

## Members and roles

- **`ZoneOffset`** — a fixed offset from UTC (total-seconds in ±18h). Pure **stack value
  type**; every factory returns a fresh `stack` value by copy. No tz database, no DST.
- **`ZoneId`** — a *region* identifier (IANA name). A **reference object** created with
  `ZoneId.of(...)` returning **owned `#ZoneId`**. It derives a `ZoneOffset` per-instant
  from the system tz database, so it honours DST.
- **`ZonedDateTime`** — the result: an `Instant` (epochSecond+nano) paired with a fixed
  `offsetSeconds`. Pure **stack value type**. Stored canonically as the UTC instant, so it
  round-trips losslessly via `toInstant()`; wall-clock getters derive fields by shifting.

## The two flows

### Fixed offset (value path)
`ZoneOffset` carries no database, so YOU supply the offset. The `ZonedDateTime` keeps
exactly that offset forever — `plus(Duration)` does NOT re-resolve DST.

```cajeta
import cajeta.time.Instant;
import cajeta.time.LocalDateTime;
import cajeta.time.ZoneOffset;
import cajeta.time.ZonedDateTime;
import cajeta.lang.String;

// instant -> zoned (you already know it is observed at -05:00)
stack ZoneOffset off = ZoneOffset.ofHours(-5);
stack ZonedDateTime a = ZonedDateTime.ofInstant(Instant.ofEpochSecond(1780000000), off);

// wall-clock fields -> zoned (these fields ARE the local time at +05:30)
stack ZonedDateTime b =
    ZonedDateTime.ofLocal(LocalDateTime.of(2026, 6, 5, 9, 30),
                          ZoneOffset.ofHoursMinutes(5, 30));
String text #= b.iso();   // owned: "2026-06-05T09:30+05:30"
```

`ofInstant(instant, offset)` shifts the UTC instant by the offset to expose wall-clock
getters; the instant is unchanged. `ofLocal(ldt, offset)` does the inverse — it treats the
`LocalDateTime` as already-local and computes the underlying UTC instant. Round-trip:
`ofInstant(i, off).toInstant() == i`; `ofLocal(ldt, off).toInstant()` is `ldt` minus the
offset.

### Region id (reference path, DST-aware)
A `ZoneId` looks up the correct offset *for the given instant*, so summer and winter
instants in the same zone resolve to different offsets.

```cajeta
import cajeta.time.Instant;
import cajeta.time.ZoneId;
import cajeta.time.ZoneOffset;
import cajeta.time.ZonedDateTime;

ZoneId ny #= ZoneId.of("America/New_York");
ZoneOffset summer = ny.offsetAt(Instant.ofEpochSecond(1625140800)); // EDT -14400
ZonedDateTime zdt = ny.resolve(Instant.ofEpochSecond(0));           // local 1969-12-31T19:00
```

`offsetAt(instant)` returns a `stack ZoneOffset`; `resolve(instant)` is the shortcut for
`ofInstant(instant, offsetAt(instant))` and returns a `stack ZonedDateTime`. There is **no
`ofLocal` analogue on `ZoneId`** — a region cannot turn wall-clock fields into an instant
here (the offset would itself depend on the unknown instant), so for the wall-clock
direction you must use the fixed-offset `ZonedDateTime.ofLocal`.

## Ownership & lifecycle across the boundary

- `ZoneOffset` and `ZonedDateTime` are stack values — copied across calls, nothing to free,
  no `close()`. Factories (`ofHours`, `ofInstant`, `ofLocal`, `utc`, …) return by value.
- `ZoneId.of(...)` / `ZoneId.utc()` return **owned `#ZoneId`** — you own the handle and are
  responsible for it. The id `String` it wraps is **borrowed and stored directly** (strings
  are process-lifetime), so the caller need not keep the argument alive.
- `iso()` on `ZoneOffset` and `ZonedDateTime` returns an **owned `#String`**.

## Errors and what these types do NOT do

- `ZoneOffset` factories throw `DateTimeException` outside ±18h, and `ofHoursMinutes`
  throws on a sign mismatch between `hours` and `minutes` (e.g. `(-3, 30)`).
- `ZoneId.of(...)` does **not** validate the name. The name is only checked when
  `offsetAt`/`resolve` run the native tz lookup; an unknown or unavailable zone throws
  `DateTimeException` there. `UTC`/`GMT`/`Z`/`Etc/UTC` resolve to zero offset without
  touching the filesystem (safe in static builds with no tz database).
- `ZonedDateTime.plus(Duration)` keeps the original `offsetSeconds`; it does **not**
  re-resolve a region's DST. To track DST across a shift, re-`resolve` the new instant
  through the `ZoneId`.
- Comparison split: `ZonedDateTime.compareTo`/`isBefore`/`isAfter` order by the underlying
  **instant only** (two zoned values at different offsets but the same instant compare
  equal), whereas `operator==` additionally distinguishes the offset.

See the `cajeta/time/Instant` and `cajeta/time/LocalDateTime` skills for building the
inputs.
