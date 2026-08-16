---
id: codec-csv-overview
applies-to: [cajeta/codec/csv]
title: CSV codec neighborhood — Csv facade vs CsvReader/CsvWriter vs CsvIndex
description: Route CSV work in cajeta.codec.csv — typed Csv.parse facade, streaming reader/writer, SIMD index, and the error type.
---

# cajeta.codec.csv — neighborhood map

This package reads and writes RFC-4180 CSV. Pick the access point by task:

| Want to | Start with | Note |
| --- | --- | --- |
| Bind CSV bytes straight to a typed `T` / `T[]` | `Csv.parse<T>(...)` | Tier-1 compiler synthesizer; no value tree |
| Walk rows/fields yourself (untyped) | `CsvReader` | streaming over a `CsvIndex` |
| Produce CSV bytes | `CsvWriter` | RFC-4180 quoting, exact inverse of the reader |
| Just the structural delimiter offsets | `CsvIndex.build(...)` | low-level; `CsvReader` already drives it |
| Catch a typed-parse failure | `CsvParseException` | the only error type here |

There is **no DOM / value-tree** (unlike a generic parser) and **no file I/O** — every
type works on an in-memory `int8[]` byte buffer; read/write files with `cajeta.io`.

## Entry-point types vs support types

- **Entry points** (you instantiate or call): `Csv` (static facade, never
  constructed), `CsvReader` (`heap CsvReader(buf, n)`), `CsvWriter`
  (`heap CsvWriter()`).
- **Support**: `CsvIndex` (static; stage-1 SIMD index — a building block under
  `CsvReader`), `CsvParseException` (thrown value, see `cajeta.error.RecoverableException`).

## Collaboration / SIMD-index parse flow

`CsvReader(buf, n)` calls `CsvIndex.build(buf, n, idx)` **once** in its constructor:
a `Vector<int8,16>` SIMD scan appends the byte offsets of *structural* `,` and `\n`
(those NOT inside a quoted field — quote-parity prefix-xor masks them out) into the
reader's `idx` array, returning the count. Because CSV's only escape is the doubled
`""` (which toggles quote state out-and-back), every full 16-byte block takes the SIMD
path; only the `<16` tail is scalar (`CsvIndex.tailAppend`). After that, `nextRow()`
slices rows/fields by pure offset arithmetic against `idx`; only `field(i)` touches
bytes, and only to unquote a quoted field.

The typed `Csv.parse<T>` synthesizer layers on top of this same reader: the compiler
(Phase 4b, `MethodTemplateInstantiator`) walks `T`'s fields and emits a per-`T`
header→field bind over `CsvReader`, decoding values with the static helpers
`CsvReader.parseI64 / parseF64 / parseBool`. Header matching uses field name,
`@CsvColumn("name")`, `@CsvNamingStrategy`, or `@CsvAlias`; unmatched columns are
projected away; an unmatched field stays default unless `@CsvRequired`. Supported field
types: `int32` / `int64` / `float64` / `boolean` / `String`.

## Ownership / lifecycle

- `CsvReader(int8[] buf, int64 n)` **borrows** `buf` — it slices into it and does not
  copy or free it; keep `buf` alive for the reader's lifetime.
- `CsvReader.field(int32 i)` returns `#int8[]` — a **freshly heap-allocated, caller-owned**
  value buffer (quotes stripped, `""` collapsed). Each call allocates.
- `CsvWriter.toBytes()` returns `#int8[]` — a **caller-owned** copy of the accumulated
  bytes; the writer keeps its own internal buffer.
- `CsvParseException` is `heap`-thrown; see `cajeta/error/RecoverableException`.
- No `close()`/dispose on any type — they are plain heap objects.

## Worked example — read with the streaming reader

```cajeta
import cajeta.codec.csv.CsvReader;

// buf holds "a,b\nc,d,e" (one record per line).
CsvReader r = heap CsvReader(buf, (int64) buf.count());
int32 rows = 0;
while (r.nextRow()) {            // false at EOF; no phantom trailing row
    int32 fc = r.fieldCount();   // valid only after nextRow() returns true
    int8[] f0 #= r.field(0);      // #int8[] — caller owns this value buffer
    rows = rows + 1;
}
```

## Worked example — typed facade and writer

```cajeta
import cajeta.codec.csv.Csv;
import cajeta.codec.csv.CsvWriter;

Trade[] trades = Csv.parse<Trade[]>(bytes, (int64) bytes.count());

CsvWriter w = heap CsvWriter();
w.writeField(fa); w.writeField(fb); w.endRow();   // ',' between fields, '\n' ends row
int8[] out #= w.toBytes();        // #int8[] — caller-owned
```

## Package invariants / gotchas

- `Csv` is **never instantiated** — it is a static facade. Its method *body* is a
  failsafe: if the synthesizer does not engage it throws `CsvParseException` rather than
  returning silently-zeroed data, so a thrown failsafe means the compile-time bind missed.
- `nextRow()` must return true before `fieldCount()` / `field(i)` are meaningful.
- The reader trims a CRLF `\r` from record ends and never emits an extra empty row for a
  trailing terminator; `CsvWriter` writes bare `\n` line endings.
- v1 primitive decoders (`parseI64/parseF64/parseBool`) are **permissive** — non-digit
  bytes are skipped, not rejected (fail-loud is a later phase). `parseBool` is true iff
  the value starts with `t`/`T`/`1`.
- `CsvWriter` buffer growth currently leaks the old array on reassign (known, parked) —
  immaterial for bounded output.

For per-type detail drop to the class skills for `CsvReader`, `CsvWriter`, `CsvIndex`,
and `Csv`; library-wide codec conventions live in the `cajeta.codec` library skill.
