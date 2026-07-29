# Clock

`cajeta.time.Clock` — the static "what time is it" surface, modeled on the
clock-access parts of `java.time.Clock` / `System`. `nanoTime()` is a monotonic
counter (`CLOCK_MONOTONIC`) for measuring elapsed intervals — it is not
wall-clock time and is unaffected by NTP adjustments. `millisTime()` is
wall-clock milliseconds since the Unix epoch (`CLOCK_REALTIME`) and can jump if
the system clock is adjusted. `now()` reads the wall clock as an
[Instant](Instant.md). All members are static; there is nothing to construct.

```cajeta
int64 start = Clock.nanoTime();
Instant moment = Clock.now();
int64 elapsedNanos = Clock.nanoTime() - start;
```

## Methods

| Signature | |
|---|---|
| `static int64 nanoTime()` ⚑ | Monotonic nanosecond counter for interval measurement (CLOCK_MONOTONIC) |
| `static int64 millisTime()` | Wall-clock milliseconds since the Unix epoch (CLOCK_REALTIME) |
| `static Instant now()` ⚑ | The current instant from the system wall clock (millisecond resolution) |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/Clock.cajeta`](../../../runtime/src/cajeta/time/Clock.cajeta)
- [Instant](Instant.md), [Duration](Duration.md) — for the moments and spans a clock read produces
