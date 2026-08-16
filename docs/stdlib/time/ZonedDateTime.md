# ZonedDateTime

`cajeta.time.ZonedDateTime` — a date-time with a fixed UTC offset, modeled on
the offset-backed subset of `java.time.ZonedDateTime` (region
[ZoneId](ZoneId.md) lookups via the system tz database resolve to an offset; a
[ZoneOffset](ZoneOffset.md) is the zone here). Stored canonically as the
underlying UTC instant (`int64 epochSecond` + `int32 nano`) plus
`int32 offsetSeconds`, so it round-trips losslessly to and from
[Instant](Instant.md); wall-clock fields are derived by shifting the instant by
the offset. A pure stack value type.

```cajeta
ZoneOffset off = ZoneOffset.ofHours(-5);
LocalDateTime ldt = LocalDateTime.of(2026, 6, 5, 9, 30);
ZonedDateTime zdt = ZonedDateTime.ofLocal(ldt, off);
int32 hour = zdt.getHour();   // 9 — wall-clock, at -05:00
String text #= zdt.iso();      // "2026-06-05T09:30-05:00"
```

## Methods

| Signature | |
|---|---|
| `ZonedDateTime(int64 epochSecond, int32 nano, int32 offsetSeconds)` | Wrap a raw UTC instant at a fixed `offsetSeconds` |
| `static ZonedDateTime ofInstant(Instant instant, ZoneOffset offset)` ⚑ | The `Instant` `instant` observed at `offset` |
| `static ZonedDateTime ofLocal(LocalDateTime ldt, ZoneOffset offset)` ⚑ | The `LocalDateTime` `ldt` interpreted as being at `offset` |
| `Instant toInstant()` | The underlying UTC instant |
| `ZoneOffset getOffset()` | The offset from UTC |
| `LocalDateTime toLocalDateTime()` | The wall-clock date-time at this zone's offset |
| `int64 getEpochSecond()` | Seconds since the epoch of the underlying instant |
| `int32 getNano()` | Nanosecond-of-second of the underlying instant |
| `int32 getYear()` | Wall-clock proleptic year at this offset |
| `int32 getMonthValue()` | Wall-clock month-of-year, 1..12 |
| `int32 getDayOfMonth()` | Wall-clock day-of-month |
| `int32 getHour()` | Wall-clock hour-of-day, 0..23 |
| `int32 getMinute()` | Wall-clock minute-of-hour, 0..59 |
| `int32 getSecond()` | Wall-clock second-of-minute, 0..59 |
| `ZonedDateTime plus(Duration d)` | Advanced by a `Duration` (keeps the same offset) |
| `int32 compareTo(ZonedDateTime other)` | Total order by the underlying instant |
| `boolean isBefore(ZonedDateTime other)` | True iff this precedes `other` on the timeline |
| `boolean isAfter(ZonedDateTime other)` | True iff this is after `other` on the timeline |
| `int64 hash()` | Content hash mixing the instant and offset |
| `#String iso()` | ISO-8601 `yyyy-MM-ddTHH:mm:ss[.nnnnnnnnn]` followed by `Z` or `±HH:mm` |
| `static boolean operator== (ZonedDateTime a, ZonedDateTime b)` | Value equality — `a == b` |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/ZonedDateTime.cajeta`](../../../runtime/src/cajeta/time/ZonedDateTime.cajeta)
- [Instant](Instant.md), [LocalDateTime](LocalDateTime.md), [ZoneOffset](ZoneOffset.md), [ZoneId](ZoneId.md)
