# Duration

`cajeta.time.Duration` — an immutable, signed time-based amount with nanosecond
precision, modeled on `java.time.Duration` but backed by a single `int64`
nanosecond count (range roughly ±292 years). Negative durations are valid —
they arise from `Instant.between` when the end precedes the start, and from
`negated()`. All factories and arithmetic return fresh stack values; equality
and ordering are by total nanoseconds. Contrast with [Period](Period.md), the
calendar-based amount.

```cajeta
Duration d = Duration.ofSeconds(90L);
Duration half = Duration.ofMillis(500L);
Duration total = d.plus(half);
int64 ms = total.toMillis();              // 90500
boolean shorter = d.compareTo(total) < 0; // true
```

## Methods

| Signature | |
|---|---|
| `Duration(int64 nanos)` | Construct directly from a nanosecond count |
| `static Duration ofNanos(int64 nanos)` | Construct from a raw nanosecond count |
| `static Duration ofMillis(int64 millis)` ⚑ | Construct from milliseconds |
| `static Duration ofSeconds(int64 seconds)` ⚑ | Construct from seconds |
| `static Duration ofMinutes(int64 minutes)` ⚑ | Construct from minutes |
| `static Duration ofHours(int64 hours)` ⚑ | Construct from hours |
| `static Duration ofDays(int64 days)` | Construct from days |
| `static Duration zero()` | The zero duration |
| `int64 toNanos()` | Total nanoseconds |
| `int64 toMillis()` | Total whole milliseconds (truncated toward zero) |
| `int64 toSeconds()` | Total whole seconds (truncated toward zero) |
| `int64 toMinutes()` | Total whole minutes (truncated toward zero) |
| `int64 toHours()` | Total whole hours (truncated toward zero) |
| `int64 toDays()` | Total whole days (truncated toward zero) |
| `int32 nanosPart()` | Nanosecond-of-second part, -999999999..999999999 (sign follows the value) |
| `Duration plus(Duration other)` | Sum, as a new stack `Duration` |
| `Duration minus(Duration other)` | Difference `this - other`, as a new stack `Duration` |
| `Duration multipliedBy(int64 scalar)` | Scale by an integer factor |
| `Duration dividedBy(int64 divisor)` | Integer division (truncates toward zero) |
| `Duration negated()` | The negation of this amount |
| `Duration abs()` | The absolute value of this amount |
| `boolean isZero()` | True iff exactly zero |
| `boolean isNegative()` | True iff negative |
| `int32 compareTo(Duration other)` | Total order by nanoseconds |
| `int64 hash()` | Content hash (the nanosecond count) |
| `static boolean operator== (Duration a, Duration b)` | Value equality — `a == b` |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta),
  [AsyncDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/AsyncDemo.cajeta)
- Source: [`runtime/src/cajeta/time/Duration.cajeta`](../../../runtime/src/cajeta/time/Duration.cajeta)
- [Instant](Instant.md) — the points a `Duration` separates; [Period](Period.md) — the calendar sibling
