# LocalDate

`cajeta.time.LocalDate` — an immutable calendar date (`yyyy-MM-dd`) with no
time-of-day and no time-zone, on the proleptic Gregorian calendar, modeled on
`java.time.LocalDate`. Stored canonically as a single `int64` epoch-day count
(days since 1970-01-01; negative before it); year/month/day parts are derived
on access. A pure stack value type. Out-of-range or non-existent dates throw
`DateTimeException`, and day-of-week follows ISO-8601 (Monday = 1 … Sunday = 7).
Build one with the `of`/`ofEpochDay` factories; calendar arithmetic accepts a
[Period](Period.md), and pairing with a [LocalTime](LocalTime.md) yields a
[LocalDateTime](LocalDateTime.md).

```cajeta
LocalDate d = LocalDate.of(2026, 6, 5);
LocalDate due = d.plusDays(40L);
boolean later = due.isAfter(d);   // true
String text #= due.iso();          // "2026-07-15"
```

## Methods

| Signature | |
|---|---|
| `LocalDate(int64 epochDay)` | Wraps a raw epoch-day directly (no validation) |
| `static boolean isLeapYear(int32 year)` | True iff `year` is a Gregorian leap year |
| `static int32 lengthOfMonth(int32 year, int32 month)` | Number of days in `month` (1..12) of `year` |
| `static LocalDate of(int32 year, int32 month, int32 day)` ⚑ | A validated date |
| `static LocalDate ofEpochDay(int64 epochDay)` ⚑ | The date `epochDay` days after 1970-01-01 (negative for earlier) |
| `int32 getYear()` | Proleptic year |
| `int32 getMonthValue()` | Month-of-year, 1..12 |
| `int32 getDayOfMonth()` | Day-of-month |
| `int32 getDayOfWeek()` | ISO day-of-week: Monday = 1 … Sunday = 7 |
| `int64 toEpochDay()` | Days since 1970-01-01 |
| `boolean isLeap()` | True iff this date is in a leap year |
| `LocalDate plusDays(int64 days)` | This date plus `days` days |
| `LocalDate plusWeeks(int64 weeks)` | This date plus `weeks` weeks |
| `LocalDate plusMonths(int64 months)` | This date plus `months` months |
| `LocalDate plusYears(int64 years)` | This date plus `years` years (Feb 29 clamps to Feb 28 off leap years) |
| `LocalDate plus(Period p)` | Plus a calendar `Period`: years and months applied together (day clamped to the resulting month), then days added |
| `int32 compareTo(LocalDate other)` | Total order: earlier < later |
| `boolean isBefore(LocalDate other)` | True iff strictly before `other` |
| `boolean isAfter(LocalDate other)` | True iff strictly after `other` |
| `int64 hash()` | Content hash of the epoch-day |
| `#String iso()` | ISO-8601 `yyyy-MM-dd` (year padded to at least 4 digits; leading `-` for negative years) |
| `static boolean operator== (LocalDate a, LocalDate b)` | Value equality — `a == b` |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/LocalDate.cajeta`](../../../runtime/src/cajeta/time/LocalDate.cajeta)
- [LocalTime](LocalTime.md), [LocalDateTime](LocalDateTime.md), [Period](Period.md)
