---
id: time-DateTimeFormatter
applies-to: [cajeta/time/DateTimeFormatter]
title: DateTimeFormatter — render temporal values to text (the formatting entry point)
description: Immutable strftime/FormatStyle formatter for LocalDate/Time/DateTime/ZonedDateTime; ofStandard/ofPattern build it, format* return owned #String. Formats only — no parsing, no fluent builder here.
---

# DateTimeFormatter

The **primary access point** for turning a temporal value into text, in the
`cajeta.time` package (library `cajeta.time`). Modeled on
`java.time.format.DateTimeFormatter` but **formatting only**. You always start here
to produce a string from a `LocalDate` / `LocalTime` / `LocalDateTime` /
`ZonedDateTime`.

Two ways to build one, then one `format*` call:

- **Standard layout** → `DateTimeFormatter.ofStandard(FormatStyle.X)` (see
  `cajeta/time/FormatStyle` for the constants and their sample outputs).
- **Custom strftime pattern** → `DateTimeFormatter.ofPattern("%Y-%m-%d %H:%M:%S")`.

## What it does NOT do (avoid the dead ends)

- **No parsing.** Despite the Java-style name and the sibling
  `cajeta/time/DateTimeParseException`, this class only renders text. There is no
  `parse(...)` here — parse text via the temporal types' own factories
  (`LocalDate.parse`, etc.), not this class.
- **No fluent builder.** A self-returning step-builder was prototyped but is deferred
  (cajeta codegen for fluent methods on a heap object isn't sound yet). `ofPattern`
  covers the same need — do not look for `appendValue`/`appendLiteral` style methods.
- **Never throws on a bad pattern.** An unknown `%` code emits a literal `%` instead
  of raising; there is no error type to catch from a `format*` call.
- **English only.** Month/weekday names (`%B`, `%A`, etc.) are English; no locale
  argument exists.

## Construction & ownership

```cajeta
public static #DateTimeFormatter ofPattern(String pattern)
public static #DateTimeFormatter ofStandard(FormatStyle style)
public DateTimeFormatter(String pattern)   // raw; prefer the factories above
```

Both factories return an **owned `#DateTimeFormatter`** — the caller owns it and is
responsible for its lifetime. `pattern` is borrowed (copied into the instance, not
transferred). Prefer the factories over the constructor.

## The methods that matter

```cajeta
public #String formatDate(LocalDate d)              // date fields only
public #String formatTime(LocalTime t)              // time-of-day only
public #String formatDateTime(LocalDateTime t)      // date + time, no offset
public #String formatZoned(ZonedDateTime z)         // date + time + offset (%z/%Z)
public #String format(DateTimeFields f)             // the underlying renderer
public String  getPattern()                         // borrowed view of the pattern
```

Every `format*` returns an **owned `#String`** the caller must manage. The receiver
and the argument are borrowed (no `#` on them) — formatting reads, never consumes.
`getPattern()` returns a **borrowed** `String` (the instance's own field; copy it to
keep it past the formatter's lifetime).

The four `format*` convenience methods just decompose their argument into a
`cajeta/time/DateTimeFields` and call `format(f)`; call `format` directly only when you
already hold a `DateTimeFields`.

## State & lifecycle

**Immutable.** It holds only the strftime pattern string and re-parses it on every
`format*` call, so one instance is freely reusable and reentrant for many values.
There is no `close()`/dispose step — it is an ordinary owned heap object.

## Offset-code caveat (`%z` / `%Z`)

The offset codes only carry a real offset through `formatZoned` (whose `DateTimeFields`
has `hasOffset` / `offsetSeconds` set). On the other `format*` paths the offset is
zero, so `%z` renders `+0000` and `%Z` renders `Z` — not an error, just the UTC
default. Use `formatZoned` when the offset matters.

## strftime codes

`%Y` year, `%y` 2-digit year, `%m` month, `%d` day, `%H` hour24, `%I` hour12,
`%M` minute, `%S` second, `%p` AM/PM, `%j` day-of-year, `%b`/`%h` short month,
`%B` full month, `%a` short weekday, `%A` full weekday, `%z` offset `±HHMM`,
`%Z` offset `Z`/`±HH:MM`, `%f` 9-digit fraction, `%L` 3-digit (milli) fraction,
`%%` literal `%`, `%n` newline, `%t` tab.

## Idiomatic example (mirrors the formatter tests)

```cajeta
import cajeta.time.LocalDateTime;
import cajeta.time.ZonedDateTime;
import cajeta.time.Instant;
import cajeta.time.ZoneOffset;
import cajeta.time.DateTimeFormatter;
import cajeta.time.FormatStyle;
import cajeta.lang.String;

// Custom pattern
LocalDateTime t = LocalDateTime.ofFull(2026, 6, 5, 14, 3, 9, 0);
#DateTimeFormatter f = DateTimeFormatter.ofPattern("%Y-%m-%d %H:%M:%S");
#String s = f.formatDateTime(t);          // "2026-06-05 14:03:09"

// Standard style, reusing the immutable formatter
#DateTimeFormatter iso = DateTimeFormatter.ofStandard(FormatStyle.ISO_LOCAL_DATE_TIME);
#String a = iso.formatDateTime(t);        // "2026-06-05T14:03:09"

// Offset codes need a ZonedDateTime
Instant i = Instant.ofEpochSecond(0);
ZoneOffset z = ZoneOffset.ofHoursMinutes(5, 30);
ZonedDateTime zdt = ZonedDateTime.ofInstant(i, z);
#String o = DateTimeFormatter.ofPattern("%H:%M %z %Z").formatZoned(zdt);  // "05:30 +0530 +05:30"
```

## Related

- `cajeta/time/FormatStyle` — the `ofStandard` enum constants and their sample outputs.
- `cajeta/time/DateTimeFields` — the decomposed-field bag `format(...)` consumes; the
  `format*` methods build it for you via its `of*` factories.
- `cajeta/time/LocalDate`, `LocalTime`, `LocalDateTime`, `ZonedDateTime` — the temporal
  values you format (and where parsing lives).
