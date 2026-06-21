---
id: time-exceptions
applies-to: [cajeta/time/DateTimeException, cajeta/time/DateTimeParseException]
title: Date/time error hierarchy (DateTimeException + DateTimeParseException)
description: The two recoverable cajeta.time exceptions — DateTimeException from of*/arithmetic on bad fields and its DateTimeParseException subtype carrying errorIndex; both catchable as DateTimeException and kept catchable by the recoverable chain-walk.
---

# Date/time exceptions

Two cooperating exception types in `cajeta.time`, both on the **recoverable** branch so a `catch` handler runs (the program does not abort):

```
RecoverableException                  (cajeta.error, catchable tier)
└── DateTimeException                 bad field / arithmetic overflow on well-formed input
    └── DateTimeParseException        malformed text; adds `errorIndex`
```

**Routing — which to catch:**
- Catch `DateTimeException` to handle *any* date/time failure — both an out-of-range field from a factory and a parse failure, because `DateTimeParseException extends DateTimeException`.
- Catch `DateTimeParseException` only when you need the `errorIndex` offset into the input string.
- You **throw** these (with `throw heap ...`) only when authoring date/time code; calling code almost always just catches them.

## Members and roles

- **`DateTimeException(String message)`** — thrown by stdlib factories and transforms (`LocalDate.of`, `LocalTime.of`, `LocalDateTime.of`, etc.) when a field is out of range (month 13, hour 24, Feb 30) or an arithmetic result escapes the representable span. One readable field that matters: inherited `message : String`. `cause` is left `0`.
- **`DateTimeParseException(String message, int64 errorIndex)`** — the subtype for text decoding (malformed ISO-8601, a field not matching its expected shape). Adds `public int64 errorIndex` — the 0-based offset where the parser noticed the problem, or `-1` when unknown. Catchable as itself **or** as `DateTimeException`.

## Why both stay catchable (the recoverable chain-walk)

Neither type carries a "recoverable" flag. Both descend from `RecoverableException`; on a throw the runtime chain-walks the thrown instance's vtable parent chain looking for `UnrecoverableException`. These never match it, so they bypass `abort()` and reach user `catch` arms. You get catchable-vs-fatal purely from the `extends` target — see `cajeta/error` skill `error-exception-hierarchy`.

## Construction & ownership

- These exceptions are thrown as **heap** values: `throw heap DateTimeException("...")`. On unwind the drop chain frees the instance (and its owned `message`) along the throw path — the caller's `catch` borrows `e` for the handler body; do not retain `e` past the handler.
- Note the constructor declares `String message` (no `#` transfer marker), unlike the base `RecoverableException(#String message)`. In practice it is called with a string literal at the throw site.
- `errorIndex` is a plain `int64` value — no ownership concern; read it directly as `e.errorIndex`.
- No `super(...)`: each constructor assigns inherited fields directly (`this.message`, `this.cause = 0`), and `DateTimeParseException` additionally sets `this.errorIndex`. Copy that pattern if you subclass.

## Worked example — catch from a factory, and the parse subtype

```cajeta
package myapp;

import cajeta.time.LocalDate;
import cajeta.time.DateTimeException;
import cajeta.time.DateTimeParseException;

public final class Demo {
    // Catching the base type handles BOTH a bad field and a parse failure.
    public static int32 safeMonth(int32 year, int32 month, int32 day) {
        try {
            stack LocalDate d = LocalDate.of(year, month, day);  // throws heap DateTimeException if invalid
            return d.getMonthValue();
        } catch (DateTimeParseException e) {                      // most specific arm first
            log(e.message);
            return -1 - (int32) e.errorIndex;                    // errorIndex available only here
        } catch (DateTimeException e) {                          // catches out-of-range field, overflow
            log(e.message);                                      // recover and keep going
            return -1;
        }
    }
}
```

Order arms specific-to-general: a `catch (DateTimeException)` placed first would also swallow `DateTimeParseException`, since the subtype *is a* `DateTimeException`.

## When to use which / what these do NOT do

- Throw `DateTimeException` for an out-of-range or non-existent field/arithmetic overflow; throw `DateTimeParseException` only on a text-decoding failure where an `errorIndex` is meaningful.
- They do **not** carry the offending field name/value as structured fields — only `message` (a human-readable string) and, on the subtype, `errorIndex`. Read `e.message`; there is no `getField()`/`getValue()` getter.
- They are **not** `UnrecoverableException` — a bare uncaught throw still unwinds as a normal recoverable exception, it does not abort.
- `DateTimeException` does **not** populate `cause` (it is `0`); these are not wrappers around a lower-level failure.
- Factories that fail throw rather than returning a sentinel/`Optional` — there is no "tryOf" variant; guard by validating input or by catching.

For the catchable/abort split and the shared `message`/`cause` fields, see `cajeta/error` (`error-exception-hierarchy`). For the factories that raise these, see the `LocalDate` / `LocalTime` / `LocalDateTime` class skills.
