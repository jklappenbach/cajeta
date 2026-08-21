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

| Message | Field | No. |
|---|---|---|
| `Trace` | `packet` | 1 |
| `TracePacket` | `clock_snapshot` | 6 |
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
