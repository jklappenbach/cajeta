---
id: time-ZoneId-offsetAt
applies-to: [cajeta/time/ZoneId.offsetAt]
title: ZoneId.offsetAt — DST-aware UTC offset for an Instant from the IANA tz database
description: Resolve the ZoneOffset in effect in a region zone at a given Instant by parsing TZif files under /usr/share/zoneinfo; throws DateTimeException for unknown/unavailable zones; UTC/GMT/Z resolve without the filesystem.
---

# ZoneId.offsetAt

Call this to get the **UTC offset that a region zone observes at a specific
moment**, honouring daylight-saving transitions — the same zone gives a different
answer in summer vs winter.

```cajeta
public ZoneOffset offsetAt(Instant instant)
```

It resolves the offset by reading the system tz database (the IANA TZif files
under `/usr/share/zoneinfo`) through a native lookup. `UTC` / `GMT` / `Z` /
`Etc/UTC` / `Etc/GMT` short-circuit to a zero offset **without touching the
filesystem**, so they work in static builds with no tz database; every other id
hits the disk.

## Return — a stack value, nothing to free

Returns a `ZoneOffset` **value type** (`stack ZoneOffset(secs)`): no `#`, no
ownership transfer, not nullable, nothing to close or free. Read its seconds with
`getTotalSeconds()` or its ISO id with `iso()` (see `cajeta/time/ZoneOffset`).

## Parameter

- `instant : Instant` — the moment to resolve at. **Only `instant.getEpochSecond()`
  is used**; the nanosecond part is ignored (offsets change on whole-second
  transition boundaries). Plain value, borrowed, not mutated.

## Preconditions & construction

Obtain the receiver from `ZoneId.of(id)` (returns `#ZoneId`, an owned heap
object) or `ZoneId.utc()`. **`ZoneId.of` does NOT validate the id** — an unknown
zone is only rejected here, when `offsetAt` runs the lookup.

## Failure modes

Throws `DateTimeException` when the zone cannot be resolved — unknown name,
unreadable/missing TZif file, or malformed TZif. (Internally the native call
returns the `INT32_MIN` sentinel and `offsetAt` rejects any result outside the
valid ±64800s / ±18h band.) `DateTimeException` extends `RecoverableException`,
so a user `catch` handler sees it rather than aborting. UTC-equivalent ids never
throw.

## Side effects

For non-UTC ids: opens `/usr/share/zoneinfo/<id>`, reads the whole file into a
heap buffer, parses it, and frees the buffer — every call. Does **not** mutate
the receiver or the `Instant`. Names with a leading `/` or containing `..` are
rejected (no path traversal); the id length must be 1..255 bytes.

## What it does NOT do

- **No caching** — each call re-opens and re-parses the TZif file. Resolve many
  instants for one zone? Expect one filesystem read per call.
- **No ZonedDateTime** — it returns only the offset. Use `ZoneId.resolve(instant)`
  (which calls `offsetAt` internally) to get a `ZonedDateTime`.
- **No zone-id validation at construction** — see preconditions above.
- **No fixed-offset parsing** — it does not accept `+05:30`-style ids; that is
  `cajeta/time/ZoneOffset`'s job. `Etc/GMT` (exactly) maps to zero, but
  `Etc/GMT+5` falls through to a file lookup.

## Example (mirrors test/time/ZoneIdTests.cpp)

```cajeta
package test;

import cajeta.time.Instant;
import cajeta.time.ZoneId;
import cajeta.time.ZoneOffset;
import cajeta.time.DateTimeException;

public final class Zit {
    public static int64 run() {
        ZoneId ny #= ZoneId.of("America/New_York");
        // 2021-07-01T12:00Z -> EDT = -4h ; 2021-01-01T12:00Z -> EST = -5h
        ZoneOffset summer = ny.offsetAt(Instant.ofEpochSecond(1625140800));
        ZoneOffset winter = ny.offsetAt(Instant.ofEpochSecond(1609502400));
        // summer.getTotalSeconds() == -14400 ; winter.getTotalSeconds() == -18000

        try {
            ZoneId.of("Not/ARealZone").offsetAt(Instant.epoch());
            return 0;                       // unreachable
        } catch (DateTimeException e) {
            return (int64) (summer.getTotalSeconds() - winter.getTotalSeconds());
        }
    }
}
```
