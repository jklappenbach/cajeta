---
id: time-formatting
applies-to: [cajeta/time/DateTimeFormatter, cajeta/time/FormatStyle, cajeta/time/DateTimeFields]
title: Rendering temporals to text with DateTimeFormatter
description: How to turn a LocalDate/Time/DateTime/ZonedDateTime into an owned #String — DateTimeFields decomposition, FormatStyle vs strftime pattern, ownership.
---

To render a temporal value to text: get a `DateTimeFormatter`, then call one of its
per-type `format*` helpers — that is the whole job.

```cajeta
import cajeta.time.DateTimeFormatter;
import cajeta.time.FormatStyle;
import cajeta.time.LocalDate;

#DateTimeFormatter fmt = DateTimeFormatter.ofStandard(FormatStyle.US_DATE);
stack LocalDate d = LocalDate.of(2026, 6, 5);
#String text = fmt.formatDate(d);     // "06/05/2026"  — you own `text` and `fmt`
```

You do **not** build a `DateTimeFields` by hand for the common path; the `format*`
helpers do that step internally.

## The three classes and how they cooperate

- **`DateTimeFormatter`** — the engine. Immutable; holds only a strftime pattern
  `String` and re-parses it on every `format` call. Get one from a factory, never
  the raw constructor.
- **`FormatStyle`** — an enum of ready-made layouts (`ISO_LOCAL_DATE_TIME`,
  `RFC_1123_DATE_TIME`, `US_DATE`, …). It is the *input to* `ofStandard`; it has no
  behaviour of its own. Each constant maps to a fixed pattern string.
- **`DateTimeFields`** — a flat heap bag of decomposed integer fields (`year`,
  `month`, … `offsetSeconds`, `hasOffset`). It is the intermediate the formatter
  actually reads; the value types (`LocalDate` etc.) are decomposed into one of
  these first. It exists because the date/time value types are single-field value
  types and can't carry the whole field set.

Call sequence: `LocalDate`/`LocalTime`/`LocalDateTime`/`ZonedDateTime`
→ `DateTimeFields.of*` (heap bag) → `DateTimeFormatter.format(fields)` → `#String`.
The `format*` helpers collapse the middle two steps.

## Build the formatter — two factories

- **`ofStandard(FormatStyle style)` → `#DateTimeFormatter`** — pick a named layout.
- **`ofPattern(String pattern)` → `#DateTimeFormatter`** — supply a strftime string
  for full control: `%Y %y %m %d %H %I %M %S %p %j %b/%h %B %a %A %z %Z %f %L %% %n %t`
  (year, 2-digit year, month, day, hour24, hour12, minute, second, AM/PM,
  day-of-year, short/full month, short/full weekday, `±HHMM` offset, `Z`/`±HH:MM`
  offset, 9- and 3-digit fraction, literal `%`, newline, tab).

Both return an **owned `#DateTimeFormatter`** that drops at scope end. A formatter is
immutable and reusable — format many values with one instance. Prefer the factories;
the public constructor `DateTimeFormatter(String)` exists but is the low-level wrapper.

## Render — match the helper to the value type

| Value type | Helper | Notes |
| --- | --- | --- |
| `LocalDate` | `formatDate(d)` | date fields only |
| `LocalTime` | `formatTime(t)` | time-of-day only; date set to placeholder `0001-01-01` |
| `LocalDateTime` | `formatDateTime(t)` | date + time, no offset |
| `ZonedDateTime` | `formatZoned(z)` | sets `hasOffset`; `%z`/`%Z` become meaningful |
| pre-built fields | `format(DateTimeFields f)` | manual path (see below) |

Each returns an **owned `#String`**. Match the `FormatStyle` to the value type:
date-only styles with `LocalDate`, time-only with `LocalTime`,
`ISO_LOCAL_DATE_TIME`/`SQL_TIMESTAMP` with `LocalDateTime`,
`ISO_OFFSET_DATE_TIME`/`ISO_INSTANT` with `ZonedDateTime`. Using a time code on a
`LocalDate` (or `%z` on a non-zoned value) does not error — it renders the zero/
placeholder value (`%z` → `+0000`, `%Z` → `Z`).

## Manual DateTimeFields path

Only needed when you want to assemble fields yourself or reuse one bag across several
formatters:

```cajeta
import cajeta.time.DateTimeFormatter;
import cajeta.time.DateTimeFields;
import cajeta.time.ZonedDateTime;

#DateTimeFields f = DateTimeFields.ofZoned(z);   // heap bag — you own it
#DateTimeFormatter fmt = DateTimeFormatter.ofPattern("%Y-%m-%dT%H:%M:%S%Z");
#String out = fmt.format(f);                      // borrows f; returns owned #String
// f drops at scope end; out and fmt are yours to drop too
```

Ownership across the boundary: `format(DateTimeFields f)` **borrows** `f` (no `#`) and
does not free it — the caller owns the bag and it drops at scope end. The `of*`
factories (`ofDate`, `ofTime`, `ofDateTime`, `ofZoned`) each return an owned
`#DateTimeFields`. The per-type `format*` helpers create their bag internally and let
it drop, so you never see it.

## What this component does NOT do

- **No parsing** — this is output only. There is no `parse` on `DateTimeFormatter`
  here; `DateTimeParseException` belongs to the (separate) parse side.
- **Never throws on a bad pattern** — an unknown `%X` code emits a literal `%`
  rather than raising; `DateTimeException` is not thrown from formatting.
- **No fluent step-builder** — a `appendValue(...).appendLiteral(...)`-style builder
  was prototyped but is deferred (codegen for self-returning fluent methods on a heap
  object isn't sound yet). Use `ofPattern` for full control instead.
- **English / ASCII only** — month and weekday names are English; no locale support.
  Out-of-range month/weekday renders `"???"`.
