---
id: codec-csv-Csv
applies-to: [cajeta/codec/csv/Csv]
title: Csv — typed Csv.parse<T> facade for binding CSV to a declared type
description: Read CSV into a typed T[] (or T) via Csv.parse<T>, with @CsvColumn/@CsvRequired field mapping; throws CsvParseException.
---

# Csv

The **main access point** for reading CSV as typed data, in package `cajeta.codec.csv`.
To turn CSV bytes into a `T[]` (whole file) or `T` (one record), call the static
`Csv.parse<T>` — you never construct `Csv` (it is `final` with only static methods).

`Csv.parse<T>` is a **Tier-1 synthesizer**, like `Json.parse<T>`: at the call site the
compiler walks `T`'s declared fields and emits a per-`T` header→field bind over
`CsvReader`. No runtime reflection, no intermediate value tree. The captured method body
is a failsafe that throws `CsvParseException` if the synthesizer fails to engage — so a
non-engaged parse fails loud rather than returning a silently default-zeroed result.

## Methods that matter

```cajeta
public static T parse<T>(int8[] bytes, int64 length);  // bind first `length` bytes to T
public static T parse<T>(String s);                     // convenience: forwards to the byte variant
```

- The element type is given by the **explicit type argument**: `Csv.parse<Trade[]>(...)`
  binds the whole file to an array; `Csv.parse<Trade>(...)` binds one record.
- Supported field types: `int32`, `int64`, `float64`, `boolean`, `String`.
- **Ownership.** `bytes` is **borrowed** — `parse` reads it and does not free it; the
  caller keeps ownership. The returned `T[]`/`T` is **freshly allocated and owned by the
  caller**. The `String` overload reads `s.bytes` / `s.byteLength` and does not consume `s`.

## Header → field matching

The first row is the header. Each header name is matched to a field of `T`,
order-independent, by this precedence:

1. field-level `@CsvColumn("name")` — explicit rename (wins over the class strategy),
2. class-level `@CsvNamingStrategy("SNAKE_CASE")` / `"KEBAB_CASE"` — maps camelCase fields,
3. the field name itself,
   plus `@CsvAlias({"uid", "user-id"})` — additional header keys accepted on read.

`@CsvIgnore` drops a field from the bind entirely. A header column with **no** matching
field is silently skipped (projection). A field with **no** matching header column is left
at its default value, **unless** marked `@CsvRequired`, which throws `CsvParseException`.

## Worked example

```cajeta
import cajeta.codec.csv.Csv;
import cajeta.lang.String;

@CsvNamingStrategy("SNAKE_CASE")
public class Trade {
    @CsvColumn("user_id") public int32 id;   // header "user_id" -> id
    @CsvRequired public float64 price;        // absent column -> CsvParseException
    public int32 firstName;                   // matches header "first_name" via strategy
}

// From bytes you already hold (buf is borrowed; rows is yours to keep):
Trade[] rows = Csv.parse<Trade[]>(buf, (int64) buf.count());

// Or straight from a String:
Trade[] more = Csv.parse<Trade[]>(csvText);
```

## Errors, lifecycle, concurrency

- Throws `CsvParseException` (a `RecoverableException`) on: a missing `@CsvRequired`
  column, a field value that fails to parse as its declared type, or the failsafe path
  (synthesizer not engaged). Its `position` is the 0-based byte offset of the problem, or
  `0` when not applicable. See skill `cajeta/codec/csv/CsvParseException`.
- No lifecycle: all-static, nothing to construct, open, or close.
- Stateless and re-entrant — each call allocates its own result.

## What this does NOT do

- **No writing** — to emit CSV use `CsvWriter`.
- **No streaming / row-by-row access** — the synthesized bind layers on `CsvReader`; drop
  to `cajeta/codec/csv/CsvReader` directly (`nextRow()` / `field(i)`) for header-less or
  row-at-a-time reading. `CsvReader.field(i)` returns an **owned** `#int8[]`.
- **No headerless parse** — the first row is consumed as the header.
- **No reflection and no value tree** — only declared fields of a statically known `T` can
  be bound; you cannot parse into a dynamic/untyped document here.
