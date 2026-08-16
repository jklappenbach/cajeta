---
id: time-ZoneId
applies-to: [cajeta/time/ZoneId]
title: ZoneId — region time zone, DST-aware offset lookup via the tz database
description: Use ZoneId.of(id) to get an owned #ZoneId, then offsetAt/resolve for a DST-aware offset; UTC-equivalent ids work with no tz db.
---

# ZoneId — region time zone with DST-aware offsets

**Access point of an otherwise value-typed package.** Reach for `ZoneId` when you
need the offset that an *IANA region* (`America/Los_Angeles`, `Europe/Paris`,
`Asia/Tokyo`) is observing at a given instant — i.e. when the answer changes with
daylight saving. It is the one **reference / heap** type here; everything else in
`cajeta.time` (`Instant`, `ZoneOffset`, `ZonedDateTime`, …) is a stack value type.

If you already have a *fixed* numeric offset that never shifts, use
`cajeta/time/ZoneOffset` directly instead — `ZoneId` exists precisely because a
region's offset is **not** fixed.

## Construct & ownership

```cajeta
import cajeta.time.ZoneId;

ZoneId la #= ZoneId.of("America/Los_Angeles");   // owned: caller frees at scope exit
ZoneId z  #= ZoneId.utc();                        // convenience for "UTC"
```

- `static #ZoneId of(String id)` / `static #ZoneId utc()` — both **return
  ownership** (`#ZoneId`, heap-allocated). Prefer these to the raw
  `ZoneId(String id)` constructor so allocation matches the rest of the package.
- The `id` `String` is **borrowed and stored directly** — not copied and not
  freed. cajeta string literals are process-lifetime, so this is safe; do not pass
  an `id` whose backing buffer you will free.
- **No validation at construction.** `of("Bogus/Zone")` succeeds; an unknown zone
  is only rejected later, when `offsetAt` runs the native lookup.
- Lifecycle: a plain heap object dropped at scope/owner exit. Nothing to `close()`.

## The methods that matter

- `String getId()` — the id it was built with (borrowed view; do not free).
- `ZoneOffset offsetAt(Instant instant)` — the UTC offset **in effect at that
  instant** (DST-aware). Returns a stack `ZoneOffset` value. Throws.
- `ZonedDateTime resolve(Instant instant)` — `instant` observed in this zone;
  applies `offsetAt`'s offset, so it throws on the same conditions. Returns a
  stack `ZonedDateTime` value.

```cajeta
import cajeta.time.ZoneId;
import cajeta.time.ZoneOffset;
import cajeta.time.ZonedDateTime;
import cajeta.time.Instant;
import cajeta.time.Clock;

stack Instant now = Clock.now();                  // current moment
ZoneId paris #= ZoneId.of("Europe/Paris");
ZoneOffset off = paris.offsetAt(now);             // +01:00 or +02:00 per DST
ZonedDateTime here = paris.resolve(now);          // same instant, Paris wall clock
```

## tz-database dependency — the sharp edge

`offsetAt` resolves real regions through a native lookup
(`__cajeta_tz_offset`) that parses the system IANA TZif files under
`/usr/share/zoneinfo`. Consequences:

- **`UTC` / `GMT` / `Z` / `Etc/UTC` resolve to a zero offset without touching the
  filesystem** — these are the only ids that work in a static build or container
  with no tz database on disk.
- Any **other** id whose zone is unknown, or any id when the tz db is unavailable,
  makes `offsetAt` (and therefore `resolve`) throw
  `cajeta/time/DateTimeException` (raised as `heap DateTimeException(...)`).
  Internally the native call returns an `INT32_MIN` sentinel and anything outside
  ±64800s (±18h) is treated as "could not resolve."

## What it does NOT do

- It does **not** validate or canonicalize the id, enumerate available zones, or
  list `/usr/share/zoneinfo` — there is no `getAvailableZoneIds()`.
- It holds no cached offset and is **immutable + stateless** beyond its id, so one
  `#ZoneId` is freely reusable across many `offsetAt`/`resolve` calls (each does a
  fresh DST-aware lookup); no per-call setup or ordering is required.
- It is not a fixed offset — for that, construct `ZoneOffset` directly.
