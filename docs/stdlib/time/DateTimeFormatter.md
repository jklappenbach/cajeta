# DateTimeFormatter

`cajeta.time.DateTimeFormatter` — formats temporal values to text, modeled on
`java.time.format.DateTimeFormatter`. Two ways to get one: `ofStandard` with a
standard `FormatStyle`, or `ofPattern` with a printf/strftime-style pattern
(`%Y` year, `%m` month, `%d` day, `%H` hour24, `%I` hour12, `%M` minute, `%S`
second, `%p` AM/PM, `%b`/`%B` short/full month, `%a`/`%A` short/full weekday,
`%z`/`%Z` offset, `%f`/`%L` 9-/3-digit fraction, `%%` literal `%`). The
formatter is immutable — it holds only its pattern string and parses it on each
`format` call. Render with `formatDate` / `formatTime` / `formatDateTime` /
`formatZoned`; output is an owned `#String`, text is English.

```cajeta
DateTimeFormatter fmt #= DateTimeFormatter.ofPattern("%Y-%m-%d %H:%M:%S");
LocalDateTime dt = LocalDateTime.of(2026, 6, 8, 14, 30);
String text #= fmt.formatDateTime(dt);   // "2026-06-08 14:30:00"
```

## Methods

| Signature | |
|---|---|
| `DateTimeFormatter(String pattern)` | Wraps a raw strftime `pattern` |
| `static #DateTimeFormatter ofPattern(String pattern)` ⚑ | A formatter for a strftime-style pattern |
| `static #DateTimeFormatter ofStandard(FormatStyle style)` ⚑ | A formatter for a standard `FormatStyle` |
| `String getPattern()` | The underlying pattern string |
| `#String format(DateTimeFields f)` | Render the `DateTimeFields` `f` by parsing this formatter's pattern |
| `#String formatDate(LocalDate d)` | Format a `LocalDate` (date fields only) |
| `#String formatTime(LocalTime t)` | Format a `LocalTime` (time-of-day fields only) |
| `#String formatDateTime(LocalDateTime t)` | Format a zone-naive `LocalDateTime` (no offset) |
| `#String formatZoned(ZonedDateTime z)` | Format a `ZonedDateTime` — the offset codes `%z` / `%Z` become available |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/DateTimeFormatter.cajeta`](../../../runtime/src/cajeta/time/DateTimeFormatter.cajeta)
- [LocalDate](LocalDate.md), [LocalTime](LocalTime.md), [LocalDateTime](LocalDateTime.md), [ZonedDateTime](ZonedDateTime.md)
