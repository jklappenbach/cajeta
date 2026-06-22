---
id: time-Period
applies-to: [cajeta/time/Period]
title: Period — immutable calendar amount (years/months/days)
description: Date-based amount type (years, months, days) for shifting LocalDate; distinct from time-based Duration.
---

# Period

A `Period` is an immutable, signed **calendar amount** of years, months, and days.
Use it to shift calendar dates (`LocalDate.plus(Period)`). It is a **support value
type**, not an entry point — you build one with the `of*` factories and pass it to a
date.

**Use `Period` when** the amount is calendar-based (months/years vary in length).
**Use `Duration` instead** (`cajeta/time/Duration`) when you need a fixed amount of
seconds/nanos (machine time). The two do not interconvert: a month is not a fixed
number of days, so `Period` never expresses a quantity in seconds.

## Construction & ownership

Pure **stack value type with copy semantics** — declare with `stack Period` and assign
freely; there is no heap allocation, no `#` transfer, and nothing to close or free.

```cajeta
import cajeta.time.Period;

stack Period p = Period.of(1, 2, 3);          // 1 year, 2 months, 3 days
stack Period years  = Period.ofYears(10);
stack Period months = Period.ofMonths(3);
stack Period weeks  = Period.ofWeeks(2);      // stored as 14 days
stack Period days   = Period.ofDays(7);
stack Period empty  = Period.zero();
stack Period direct = stack Period(2, 6, 0);  // constructor; prefer of*
```

Each field is **independent and signed**; `of(1, 2, 3)` is literally "1 year, 2 months,
3 days" with **no** cross-field normalization at construction.

## The methods that matter

All arithmetic returns a **new `Period` by value** (immutable; the receiver is never
mutated):

- `Period plus(Period other)` / `Period minus(Period other)` — field-wise.
- `Period negated()` — each field negated.
- `Period normalized()` — rolls **months into years** so the month part lands in
  `[-11, 11]`. **Days are left untouched** (a month is not a fixed number of days).
- `int64 toTotalMonths()` — `years * 12 + months` (days excluded).
- `boolean isZero()` / `boolean isNegative()` (negative iff *any* field is negative).
- `int32 getYears()` / `getMonths()` / `getDays()`.
- `operator==` (structural, all three fields) and `int64 hash()`.

```cajeta
import cajeta.time.Period;

stack Period p = Period.of(1, 2, 3);
stack Period q = p.plus(Period.ofWeeks(2));   // 1Y 2M 17D
stack Period n = q.normalized();              // months < 12 already; days stay 17
boolean none  = Period.zero().isZero();       // true
```

### iso() — the one owning method

`#String iso()` renders ISO-8601 `PnYnMnD` (`P0D` for zero; negative fields get a
leading `-`, e.g. `P-1Y2M`). It returns a **heap `String` whose ownership transfers to
the caller** — bind it with `#String` and the caller is responsible for it.

```cajeta
import cajeta.time.Period;

#String text = Period.of(1, 2, 3).iso();      // "P1Y2M3D"
```

## Consumed by LocalDate

`Period` crosses no ownership boundary when passed to a date — it is taken by value.

```cajeta
import cajeta.time.LocalDate;
import cajeta.time.Period;

stack LocalDate d   = LocalDate.of(2026, 1, 31);
stack LocalDate due = d.plus(Period.of(0, 1, 0));   // years+months first, then days
```

`LocalDate.plus(Period)` applies years+months together (**clamping the day to the
resulting month** — Jan 31 + 1 month → Feb 28/29), then adds `getDays()`. For a single
unit prefer the dedicated shifters `LocalDate.plusMonths`/`plusYears`/`plusDays` (they
take a plain `int64`, not a `Period`).

## What it does not do

- **No automatic day/month normalization** — neither at construction nor in
  `normalized()` (which only rolls months→years). `ofWeeks` is the only "unit" that
  collapses (into days).
- **No total-days / total-seconds accessor** — there is no `toDays()`/`toSeconds()`,
  because calendar months are variable-length. Reach for `Duration` for fixed time.
- **No date arithmetic of its own** — a `Period` does not know about a date; apply it
  via `LocalDate.plus(Period)`. There is no `Period.between(date, date)` here.
- **No exceptions** — every method is total over its `int32`/`int64` field math.
