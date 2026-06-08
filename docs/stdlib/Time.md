# `cajeta.time`

Modeled on `java.time` (JSR-310), pared down for cajeta's needs.
Status: **designed, not implemented**. No runtime stdlib files exist
yet. Tracked in Features.md.

## `Clock` — static "what time is it" surface

```cajeta
public final class Clock {
    // Monotonic, ns precision, for interval measurement. Backed by
    // CLOCK_MONOTONIC on Linux; not adjustable by NTP / leap seconds.
    public static int64 nanoTime();

    // Wall clock, ms precision. Backed by CLOCK_REALTIME. Can jump
    // backward if the system clock is adjusted.
    public static int64 millisTime();

    // Current instant from the system clock.
    public static Instant now();
}
```

### Example

```cajeta
import cajeta.time.Clock;

int64 start = Clock.nanoTime();
expensiveWork();
int64 elapsedNs = Clock.nanoTime() - start;
```

## Value types — all immutable, comparable, hashable

| Type | Description |
|------|-------------|
| `Instant` | A moment on the UTC timeline, ns precision. Internally `int64 secondsSinceEpoch + int32 nanos`. |
| `Duration` | A time-based amount, ns precision. Negative durations OK. |
| `Period` | A calendar-based amount (years/months/days). Distinct from `Duration` because months aren't fixed. |
| `LocalDate` | `yyyy-mm-dd`, no time, no zone. |
| `LocalTime` | `hh:mm:ss.nnnnnnnnn`, no date, no zone. |
| `LocalDateTime` | `LocalDate` + `LocalTime`, zone-naive. |
| `ZoneId` | Time-zone identifier (`America/Los_Angeles`, etc.). Resolves to offsets via the system tz database. |
| `ZoneOffset` | A fixed offset from UTC (`+05:30`, `-08:00`). Subtype of `ZoneId`. |
| `ZonedDateTime` | `LocalDateTime` + `ZoneId`. Round-trippable to/from `Instant`. |
| `DateTimeFormatter` | Pattern-based formatting + parsing (ISO-8601 / `java.time.format`). |

### Example sketch

```cajeta
import cajeta.time.Instant;
import cajeta.time.Duration;
import cajeta.time.ZoneId;

Instant now = Instant.now();
Instant later = now.plus(Duration.ofMinutes(30));
Duration elapsed = Instant.between(now, later);    // 30 minutes

ZonedDateTime here = now.atZone(ZoneId.of("America/Los_Angeles"));
```

## Open questions

- **Time zone database**: parse `/usr/share/zoneinfo` at first use vs.
  bake compact tables into the runtime. Recommended: read-from-disk
  with embedded UTC + fixed-offset fallback for static builds.
- **Leap seconds**: `Instant` ignores them (UTC-SLS convention, same
  as java.time).
- **Nanosecond precision everywhere**: Java's `Instant` is ns
  precision but many platforms only deliver ms. Document the system
  clock resolution caveat.

## Open items

All of `cajeta.time` is unimplemented. Tracked in Features.md.
