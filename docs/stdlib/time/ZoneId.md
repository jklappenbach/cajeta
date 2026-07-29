# ZoneId

`cajeta.time.ZoneId` — a geographical/region time-zone identifier (e.g.
`America/Los_Angeles`, `Europe/Paris`, `UTC`), modeled on the region subset of
`java.time.ZoneId`. Unlike a fixed [ZoneOffset](ZoneOffset.md), a `ZoneId` maps
to different offsets at different instants — it honours daylight-saving
transitions. Offsets are resolved against the system tz database
(`/usr/share/zoneinfo`); `UTC` / `GMT` / `Z` / `Etc/UTC` resolve to a zero
offset without touching the filesystem, and any other unknown or unavailable
zone makes `offsetAt` throw `DateTimeException`. Unlike the package's value
types it is a reference object, created with `ZoneId.of(...)`.

```cajeta
ZoneId utc = ZoneId.of("UTC");
Instant t0 = Instant.ofEpochSecond(1000L);
ZoneOffset off = utc.offsetAt(t0);   // +00:00
ZonedDateTime zdt = utc.resolve(t0);
```

## Methods

| Signature | |
|---|---|
| `ZoneId(String id)` | Wraps a raw IANA id; prefer the `of` factory |
| `static #ZoneId of(String id)` ⚑ | The zone with the given IANA id |
| `static #ZoneId utc()` ⚑ | The UTC zone; always resolves to a zero `ZoneOffset` without touching the tz database |
| `String getId()` | The IANA id string this zone was created with |
| `ZoneOffset offsetAt(Instant instant)` | The UTC offset in effect in this zone at `instant` (DST-aware) |
| `ZonedDateTime resolve(Instant instant)` | The `ZonedDateTime` for `instant` observed in this zone |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/ZoneId.cajeta`](../../../runtime/src/cajeta/time/ZoneId.cajeta)
- [ZoneOffset](ZoneOffset.md), [ZonedDateTime](ZonedDateTime.md), [Instant](Instant.md)
