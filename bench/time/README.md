# cajeta.time

This section measures dates, times and formatting against the date libraries of five languages.

| Page | |
|---|---|
| [Clock](Clock.md) | The static what-time-is-it surface |
| [Instant](Instant.md) | A moment on the UTC timeline at nanosecond precision |
| [Duration](Duration.md) | The signed time-based amount at nanosecond precision |
| [LocalDate](LocalDate.md) | The calendar date with no time or zone |
| [LocalTime](LocalTime.md) | Time-of-day with no date or zone |
| [LocalDateTime](LocalDateTime.md) | A date paired with a time, no zone |
| [ZonedDateTime](ZonedDateTime.md) | A date-time with a fixed UTC offset. The row applies a region zone with transitions, which is where the tz database is paid for |
| [DateTimeFormatter](DateTimeFormatter.md) | Formatting and parsing |

## Standing

Geometric mean 5.7x behind over 8 benchmarks, no wins. Clock, Instant and LocalDate are within 1.2x to 1.8x of the fastest library. Duration (14x), LocalTime (6x), the ISO formatter (13x) and region zones (114x) are the losses, none yet investigated.
