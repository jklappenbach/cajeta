# ZoneOffset

`cajeta.time.ZoneOffset` — a fixed offset from UTC, e.g. `+05:30`, `-08:00`, or
`Z` (UTC), modeled on `java.time.ZoneOffset`. Stored as a single `int32`
total-seconds value in `[-64800, 64800]` (±18 hours); a pure stack value type.
Build one through the `of*`/`utc` factories rather than the raw constructor —
they range-check and throw `DateTimeException` on bad input. Fixed offsets are
the zone backbone for [ZonedDateTime](ZonedDateTime.md); DST-aware region
lookups live in [ZoneId](ZoneId.md).

```cajeta
ZoneOffset ist = ZoneOffset.ofHoursMinutes(5, 30);
ZoneOffset pst = ZoneOffset.ofHours(-8);
boolean east = ist.compareTo(pst) > 0;   // true
String id = ist.iso();                   // "+05:30"
```

## Methods

| Signature | |
|---|---|
| `ZoneOffset(int32 totalSeconds)` | A raw offset from a `totalSeconds` value; performs no range check |
| `static ZoneOffset ofTotalSeconds(int32 totalSeconds)` ⚑ | Offset of `totalSeconds` seconds from UTC; throws `DateTimeException` outside `[-64800, 64800]` |
| `static ZoneOffset ofHours(int32 hours)` ⚑ | Offset of whole hours; throws `DateTimeException` outside ±18 |
| `static ZoneOffset ofHoursMinutes(int32 hours, int32 minutes)` ⚑ | Offset of hours and minutes; both parts must share the offset's sign |
| `static ZoneOffset utc()` ⚑ | The UTC offset (zero), the `Z` of `+00:00` |
| `int32 getTotalSeconds()` | Total offset in seconds from UTC (may be negative) |
| `int32 compareTo(ZoneOffset other)` | Ascending order by offset seconds |
| `int64 hash()` | Content hash of the offset seconds |
| `#String iso()` | ISO offset id: `Z` for UTC, else `+HH:mm` / `-HH:mm` (`:ss` appended only for a nonzero seconds part) |
| `static boolean operator== (ZoneOffset a, ZoneOffset b)` | Value equality — `a == b` |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/ZoneOffset.cajeta`](../../../runtime/src/cajeta/time/ZoneOffset.cajeta)
- [ZoneId](ZoneId.md), [ZonedDateTime](ZonedDateTime.md), [Instant](Instant.md)
