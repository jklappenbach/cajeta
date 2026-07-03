# Instant

`cajeta.time.Instant` — an immutable moment on the UTC timeline with nanosecond
precision, modeled on `java.time.Instant`. Stored as `int64 epochSecond`
(seconds since 1970-01-01T00:00:00Z, may be negative) plus `int32 nano`
(0..999999999), the canonical normalized form so equal instants compare and
hash equally. A pure stack value type: arithmetic is via
[Duration](Duration.md), and calendar breakdown is done by projecting through
[LocalDateTime](LocalDateTime.md) at a [ZoneOffset](ZoneOffset.md). Implements
`Comparable`, so instants sort chronologically.

```cajeta
Instant t0 = Instant.ofEpochSecond(1000L);
Duration hour = Duration.ofHours(1L);
Instant t1 = t0.plus(hour);
Duration gap = Instant.between(t0, t1);   // 1h
boolean earlier = t0.isBefore(t1);        // true
```

## Methods

| Signature | |
|---|---|
| `Instant(int64 epochSecond, int32 nano)` | Raw constructor; caller keeps `nano` in 0..999999999 — prefer the `ofEpoch*` factories, which normalize |
| `static Instant ofEpochSecond(int64 epochSecond)` ⚑ | The instant `epochSecond` seconds after the epoch, zero nanos |
| `static Instant ofEpochSecondAdjust(int64 epochSecond, int64 nanoAdjustment)` | Seconds plus a nanosecond adjustment, normalized |
| `static Instant ofEpochMilli(int64 epochMilli)` ⚑ | The instant `epochMilli` milliseconds after the epoch |
| `static Instant epoch()` ⚑ | The epoch itself, 1970-01-01T00:00:00Z |
| `int64 getEpochSecond()` | Seconds since the epoch (may be negative) |
| `int32 getNano()` | Nanosecond-of-second, 0..999999999 |
| `int64 toEpochMilli()` | Milliseconds since the epoch |
| `Instant plusSeconds(int64 seconds)` | This instant plus `seconds` seconds |
| `Instant plusMillis(int64 millis)` | This instant plus `millis` milliseconds, normalized |
| `Instant plusNanos(int64 nanos)` | This instant plus `nanos` nanoseconds, normalized |
| `Instant plus(Duration d)` | This instant advanced by a `Duration` |
| `Instant minus(Duration d)` | This instant moved back by a `Duration` |
| `static Duration between(Instant start, Instant end)` ⚑ | Elapsed `Duration` from `start` to `end` (negative if end precedes start) |
| `int32 compareTo(Instant other)` | Total order on the UTC timeline |
| `boolean isBefore(Instant other)` | True iff strictly before `other` |
| `boolean isAfter(Instant other)` | True iff strictly after `other` |
| `int64 hash()` | Content hash mixing the two fields |
| `static boolean operator== (Instant a, Instant b)` | Value equality — `a == b` |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/Instant.cajeta`](../../../runtime/src/cajeta/time/Instant.cajeta)
- [Clock](Clock.md) — produces the current `Instant`; [ZonedDateTime](ZonedDateTime.md) — an `Instant` seen through an offset
