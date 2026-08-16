# LocalDateTime

`cajeta.time.LocalDateTime` — an immutable date-time without a time-zone: a
[LocalDate](LocalDate.md) paired with a [LocalTime](LocalTime.md)
(`yyyy-MM-ddTHH:mm:ss.nnnnnnnnn`), modeled on `java.time.LocalDateTime`. Stored
canonically as `int64 epochDay` + `int64 nanoOfDay`; a pure stack value type.
Arithmetic carries correctly between the time and date parts — adding hours can
roll the date forward. Combine with a [ZoneOffset](ZoneOffset.md) to obtain an
[Instant](Instant.md) or [ZonedDateTime](ZonedDateTime.md).

```cajeta
LocalDateTime dt = LocalDateTime.of(2026, 6, 8, 14, 30);
LocalDateTime later = dt.plusHours(12L);
int32 day = later.getDayOfMonth();   // 9 — carried into the next day
String text #= later.iso();           // "2026-06-09T02:30"
```

## Methods

| Signature | |
|---|---|
| `LocalDateTime(int64 epochDay, int64 nanoOfDay)` | Low-level constructor from canonical fields |
| `static LocalDateTime of(int32 year, int32 month, int32 day, int32 hour, int32 minute)` ⚑ | From date and time parts down to minutes (seconds and nanos zero) |
| `static LocalDateTime ofFull(int32 year, int32 month, int32 day, int32 hour, int32 minute, int32 second, int32 nano)` ⚑ | From date and time parts down to nanoseconds |
| `static LocalDateTime ofDateTime(LocalDate date, LocalTime time)` ⚑ | Combine a `LocalDate` and a `LocalTime` |
| `int32 getYear()` | Proleptic year |
| `int32 getMonthValue()` | Month-of-year, 1..12 |
| `int32 getDayOfMonth()` | Day-of-month |
| `int32 getDayOfWeek()` | ISO day-of-week: Monday = 1 … Sunday = 7 |
| `int32 getHour()` | Hour-of-day |
| `int32 getMinute()` | Minute-of-hour |
| `int32 getSecond()` | Second-of-minute |
| `int32 getNano()` | Nanosecond-of-second |
| `LocalDate toLocalDate()` | The date part as a `LocalDate` |
| `LocalTime toLocalTime()` | The time part as a `LocalTime` |
| `LocalDateTime plusDays(int64 days)` | This date-time plus `days` days |
| `LocalDateTime plusNanos(int64 nanos)` | Plus `nanos` nanoseconds, carrying into the date |
| `LocalDateTime plusSeconds(int64 seconds)` | Plus `seconds` seconds, carrying into the date |
| `LocalDateTime plusMinutes(int64 minutes)` | Plus `minutes` minutes, carrying into the date |
| `LocalDateTime plusHours(int64 hours)` | Plus `hours` hours, carrying into the date |
| `int64 toEpochSecond(ZoneOffset offset)` | Seconds since the epoch when read at a `ZoneOffset` |
| `Instant toInstant(ZoneOffset offset)` | The `Instant` this local date-time denotes at the given `ZoneOffset` |
| `int32 compareTo(LocalDateTime other)` | Total order: date part, then time part |
| `boolean isBefore(LocalDateTime other)` | True iff strictly before `other` |
| `boolean isAfter(LocalDateTime other)` | True iff strictly after `other` |
| `int64 hash()` | Content hash mixing the date and time |
| `#String iso()` | ISO-8601 `yyyy-MM-ddTHH:mm:ss[.nnnnnnnnn]` |
| `static boolean operator== (LocalDateTime a, LocalDateTime b)` | Value equality — `a == b` |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/LocalDateTime.cajeta`](../../../runtime/src/cajeta/time/LocalDateTime.cajeta)
- [LocalDate](LocalDate.md), [LocalTime](LocalTime.md), [ZonedDateTime](ZonedDateTime.md)
