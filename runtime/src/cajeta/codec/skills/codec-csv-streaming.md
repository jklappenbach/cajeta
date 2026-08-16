---
id: codec-csv-streaming
applies-to: [cajeta/codec/csv/CsvReader, cajeta/codec/csv/CsvWriter, cajeta/codec/csv/CsvIndex]
title: Low-level streaming CSV read/write (CsvIndex + CsvReader + CsvWriter)
description: Build/walk an RFC-4180 CSV byte stream field-by-field with CsvReader and CsvWriter; CsvIndex is the structural index they ride on.
---

# Streaming CSV (RFC-4180)

For reading raw CSV bytes into per-field `int8[]` values, or writing fields back
out to CSV bytes, work with **`CsvReader`** and **`CsvWriter`**. They operate on
byte buffers, not strings, and one field at a time. **`CsvIndex`** is the SIMD
structural index underneath — `CsvReader` builds it for you, so you rarely touch
it directly.

This is the *untyped* layer. To bind rows to a declared struct (`Trade[] t =
Csv.parse<Trade[]>(...)`) use the typed facade `cajeta/codec/csv/Csv` instead —
that is a separate Tier-1 synthesizer component and the only place
`CsvParseException` is raised. Nothing in this streaming layer throws.

## Members and roles

- **`CsvReader`** — entry point for reading. `heap CsvReader(buf, n)` builds the
  index once, then `nextRow()` / `fieldCount()` / `field(i)` walk records.
- **`CsvWriter`** — entry point for writing. `heap CsvWriter()`, then
  `writeField` / `endRow` repeatedly, then `toBytes()`.
- **`CsvIndex`** — support type, all `static`. `build(b, n, idx)` scans bytes and
  appends the offsets of structural `,` and `\n` (those outside quotes) into a
  caller-supplied `int32[]`, returning the count. Use directly only if you need
  raw delimiter offsets; otherwise `CsvReader` calls it internally.

## Collaboration / call sequence

`CsvReader`'s constructor calls `CsvIndex.build` and keeps the index plus two
scratch offset arrays; it then slices fields by offset arithmetic (only quoted
fields are walked byte-by-byte to unquote). Read loop:

1. `heap CsvReader(buf, n)` — `n` is the valid byte length (pass it explicitly;
   it is *not* read from `buf.count()`).
2. `nextRow()` → `boolean` — advances to the next record; `false` at EOF. A
   trailing record terminator does **not** yield a phantom empty row.
3. `fieldCount()` → `int32` — valid only after a `nextRow()` that returned true.
4. `field(i)` → owned `int8[]` — the i-th field's *value* bytes: outer quotes
   stripped, doubled `""` collapsed to one `"`, trailing `\r` of a CRLF
   terminator trimmed.
5. Optionally decode with the static `CsvReader.parseI64 / parseF64 / parseBool`.

`CsvWriter` is the exact inverse of `field()` decoding, so reader-over-writer
round-trips. Write loop: `writeField(value)` (prefixes `,` except first in row,
quotes+escapes only when the value contains `,` `"` `\n` or `\r`) → `endRow()`
(appends `\n`, resets to row start) → repeat → `toBytes()`.

## Ownership / lifecycle

- **`CsvReader(buf, n)` borrows `buf`** — it is stored and read by every
  `field(i)` call, so `buf` must outlive the reader. The constructor allocates
  its own index/scratch arrays internally.
- **`field(i)` returns an owned `#int8[]`** — a freshly heap-allocated copy on
  *every* call (even unquoted fields are copied). The caller frees it.
- **`writeField(value)` borrows `value`** (copied into the writer's buffer).
- **`toBytes()` returns an owned `#int8[]`** copy of the bytes so far; the writer
  stays usable. Caller frees it.
- **`CsvIndex.build(b, n, idx)`** borrows both `b` and `idx`; it *writes into*
  `idx`, which the caller must size to hold at least `n` entries (`CsvReader`
  uses `n + 16`). Returns the count written.
- No `close()`/disposal on either reader or writer; just free the owned `int8[]`
  values they hand back.

## What it does NOT do

- No typed/struct binding, no header→field mapping — that is `Csv.parse<T>`.
- The decoders `parseI64/parseF64/parseBool` are **permissive**: non-numeric
  bytes are skipped, not rejected — they never throw. `parseBool` is true iff the
  value starts with `t`, `T`, or `1`. Fail-loud parsing lives in `Csv.parse<T>`.
- `CsvWriter` currently **leaks the old buffer when it grows** (drop-on-reassign
  gap; immaterial for bounded output) — a known, parked limitation.

## Worked example

```cajeta
import cajeta.codec.csv.CsvReader;
import cajeta.codec.csv.CsvWriter;
import cajeta.lang.String;

String text = "sym,qty\nAAPL,100\nMSFT,50\n";
int8[] buf = text.bytes;                            // borrowed by the reader
CsvReader r = heap CsvReader(buf, (int64) text.byteLength);

CsvWriter w = heap CsvWriter();
r.nextRow();                                        // consume the header row
while (r.nextRow()) {
    int8[] sym #= r.field(0);                        // owned copy, e.g. "AAPL"
    int8[] qty #= r.field(1);
    int64 q = CsvReader.parseI64(qty);              // permissive int parse
    w.writeField(sym);                              // borrows sym
    w.writeField(qty);
    w.endRow();
    // free sym, qty here when done with them
}
int8[] out #= w.toBytes();                           // owned CSV bytes
```

For raw delimiter offsets without a reader, call `CsvIndex.build` directly:

```cajeta
import cajeta.codec.csv.CsvIndex;

int32[] idx = heap int32[n + 16];                   // must hold >= n entries
int32 cnt = CsvIndex.build(buf, (int64) n, idx);    // cnt structural positions
```
