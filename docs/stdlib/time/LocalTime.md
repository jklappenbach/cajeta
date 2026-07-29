# LocalTime

`cajeta.time.LocalTime` — an immutable time-of-day with nanosecond precision
and no date or time-zone (`HH:mm:ss.nnnnnnnnn`), modeled on
`java.time.LocalTime`. Stored canonically as a single `int64` nanosecond-of-day
in `[0, 86399999999999]`; the hour/minute/second/nano parts are derived on
access. A pure stack value type; an out-of-range field throws
`DateTimeException`. Arithmetic wraps within a 24-hour day — adding an hour to
23:30 yields 00:30; no day overflow is carried out (use
[LocalDateTime](LocalDateTime.md) for that).

```cajeta
LocalTime start = LocalTime.ofHms(23, 30, 0);
LocalTime end = start.plusMinutes(45L);
int32 h = end.getHour();          // 0 — wrapped past midnight
String text = end.iso();          // "00:15"
```

## Methods

| Signature | |
|---|---|
| `LocalTime(int64 nanoOfDay)` | Wraps a raw nanosecond-of-day without range checking; prefer `ofNanoOfDay` |
| `static LocalTime ofHmsn(int32 hour, int32 minute, int32 second, int32 nano)` ⚑ | `hh:mm:ss.nnnnnnnnn` — validates every field, throwing `DateTimeException` if out of range |
| `static LocalTime of(int32 hour, int32 minute)` ⚑ | `hh:mm` |
| `static LocalTime ofHms(int32 hour, int32 minute, int32 second)` ⚑ | `hh:mm:ss` |
| `static LocalTime midnight()` | 00:00, the start of the day |
| `static LocalTime noon()` | 12:00 |
| `static LocalTime ofNanoOfDay(int64 nanoOfDay)` | The time `nanoOfDay` nanoseconds after midnight (validated) |
| `static LocalTime ofSecondOfDay(int64 secondOfDay)` | The time `secondOfDay` seconds after midnight (validated) |
| `int32 getHour()` | Hour-of-day |
| `int32 getMinute()` | Minute-of-hour |
| `int32 getSecond()` | Second-of-minute |
| `int32 getNano()` | Nanosecond-of-second |
| `int64 toNanoOfDay()` | Nanoseconds since midnight |
| `int64 toSecondOfDay()` | Whole seconds since midnight (drops the nanosecond part) |
| `LocalTime plusNanos(int64 nanos)` | This time plus `nanos`, wrapping within the day |
| `LocalTime plusSeconds(int64 seconds)` | This time plus `seconds`, wrapping within the day |
| `LocalTime plusMinutes(int64 minutes)` | This time plus `minutes`, wrapping within the day |
| `LocalTime plusHours(int64 hours)` | This time plus `hours`, wrapping within the day |
| `int32 compareTo(LocalTime other)` | Total order by nanosecond-of-day |
| `boolean isBefore(LocalTime other)` | True iff strictly before `other` |
| `boolean isAfter(LocalTime other)` | True iff strictly after `other` |
| `int64 hash()` | Content hash of the nanosecond-of-day |
| `#String iso()` | ISO-8601: `HH:mm`, `HH:mm:ss` with seconds, or `HH:mm:ss.nnnnnnnnn` with nanos |
| `static boolean operator== (LocalTime a, LocalTime b)` | Value equality — `a == b` |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/LocalTime.cajeta`](../../../runtime/src/cajeta/time/LocalTime.cajeta)
- [LocalDate](LocalDate.md), [LocalDateTime](LocalDateTime.md)
