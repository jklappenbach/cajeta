# perfetto_trace.proto — vendored

Amalgamated Perfetto trace protobuf schema. Vendored so the field numbers the
trace writer emits can be checked against the real schema instead of recalled,
and so they stay checkable without network access.

- **Source**: https://raw.githubusercontent.com/google/perfetto/main/protos/perfetto/trace/perfetto_trace.proto
- **Fetched**: 2026-08-21
- **sha256**: `c43d1658360a80a144a2d7a0a6e58a70795b0051f4e333db9d17834eab4a7a1a`
- **Size**: 682170 bytes

## Why it is here

`cajeta-profiler` emits Perfetto protobuf **directly** — no SDK, no generated
code, no build dependency (spec §7, plan 5.3.b). Nothing compiles this file. It
is a reference: the writer hand-encodes a handful of fields, and a wrong field
number produces a file that is valid protobuf and still does not load, which is
the worst failure shape available — every local test passes and the artifact is
useless.

`ide-coverage`'s sibling plan (`cajeta-profiler` Unit 11.2.a) will generate
Java/Kotlin classes from this same file for the IntelliJ viewer, which is why it
is vendored at `third_party/` rather than beside the runtime.

## Fields the writer depends on (verified against this file, 2026-08-21)

Spec §7.7 limits the writer to the stable `TrackEvent` / `TrackDescriptor`
schema; anything GPU-specific is an optional addition, because those are
explicitly outside Perfetto's stability guarantee.

**`clock_snapshot` is materialized lazily** (measured against trace_processor
v57.2, 2026-08-21). A well-formed `ClockSnapshot` packet does NOT produce rows
in the `clock_snapshot` table on its own: the table is populated only when some
packet actually references the sequence-scoped clock through
`timestamp_clock_id`. Bisected with hand-built traces — snapshot alone: 0 rows;
snapshot + a track descriptor: 0; snapshot + a slice timestamped in the device
clock: **2**. `cajeta-profiler` converts device timestamps into the host domain
at the seam (spec §5.1.7), so its traces never reference the device clock and
the table stays empty even though the snapshot is present and correct. Do not
read an empty `clock_snapshot` as a missing or malformed snapshot.

**Clock ids carry a scoping rule, not just a number** (verified 2026-08-21, from
this file's `ClockSnapshot.Clock` comment): `[1, 63]` are builtin — `MONOTONIC`
is **3** — and `[64, 127]` are user-defined and **sequence-scoped**, valid only
within the packet sequence that emitted the snapshot. `cajeta-profiler` numbers
a device domain `64 + domain` (Unit 9, spec §7.5), which is only correct because
the writer keeps one `trusted_packet_sequence_id` for the whole file. Splitting
the writer across sequences would silently unbind every device clock from its
snapshot — the trace would still load.

| Message | Field | No. |
|---|---|---|
| `Trace` | `packet` | 1 |
| `TracePacket` | `clock_snapshot` | 6 |
| `ClockSnapshot` | `clocks` | 1 |
| `ClockSnapshot.Clock` | `clock_id` / `timestamp` / `is_incremental` / `unit_multiplier_ns` | 1 / 2 / 3 / 4 |
| `TracePacket` | `timestamp` | 8 |
| `TracePacket` | `trusted_packet_sequence_id` | 10 |
| `TracePacket` | `track_event` | 11 |
| `TracePacket` | `interned_data` | 12 |
| `TracePacket` | `sequence_flags` | 13 |
| `TracePacket` | `timestamp_clock_id` | 58 |
| `TracePacket` | `track_descriptor` | 60 |
| `TrackDescriptor` | `uuid` / `name` / `process` / `thread` / `parent_uuid` | 1 / 2 / 3 / 4 / 5 |
| `TrackEvent` | `type` | 9 |
| `TrackEvent` | `name_iid` | 10 |
| `TrackEvent` | `track_uuid` | 11 |
| `TrackEvent` | `name` | 23 |
| `TrackEvent` | `source_location_iid` | 34 |
| `TrackEvent` | `debug_annotations` | 4 |
| `TrackEvent` | `flow_ids` (**fixed64**) | 47 |
| `TrackEvent` | `terminating_flow_ids` (**fixed64**) | 48 |
| `DebugAnnotation` | `int_value` / `string_value` / `name` | 4 / 6 / 10 |
| `InternedData` | `event_names` | 2 |
| `InternedData` | `source_locations` | 4 |
| `EventName` | `iid` / `name` | 1 / 2 |
| `SourceLocation` | `iid` / `file_name` / `function_name` / `line_number` | 1 / 2 / 3 / 4 |

The two flow fields are the only **fixed64** entries in the table — eight
little-endian bytes, wire type 1, not a varint. Their deprecated varint twins
(`flow_ids_old` = 36, `terminating_flow_ids_old` = 42) still exist in the
schema, so encoding a flow as a varint under field 47 produces a packet that
parses cleanly and a flow that never appears.

`TrackEvent.Type`: `TYPE_SLICE_BEGIN` = 1, `TYPE_SLICE_END` = 2,
`TYPE_INSTANT` = 3.

## Updating

Re-fetch from the URL above, update the date and sha256, and re-check the table.
A field number changing is not expected — these are wire-compatible and Perfetto
treats them as such — but the table is the thing to re-verify, not the file size.
