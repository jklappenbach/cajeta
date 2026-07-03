# Period

`cajeta.time.Period` — an immutable calendar-based amount of time (years,
months, and days), modeled on `java.time.Period`. Distinct from
[Duration](Duration.md), which measures a fixed number of seconds/nanos:
months and years are not fixed-length. Each field is independent and signed;
there is no automatic normalization between days and months — `normalized()`
only rolls months into years. A pure stack value type. Pair with
[LocalDate](LocalDate.md) to shift calendar dates.

```cajeta
Period p = Period.of(1, 2, 10);
Period w = Period.ofWeeks(2);
Period q = p.plus(w);
int64 months = q.toTotalMonths();   // 14
String text = q.iso();              // "P1Y2M24D"
```

## Methods

| Signature | |
|---|---|
| `Period(int32 years, int32 months, int32 days)` | Constructs a period directly from its three signed fields |
| `static Period of(int32 years, int32 months, int32 days)` ⚑ | A period of `years` years, `months` months and `days` days |
| `static Period ofYears(int32 years)` ⚑ | A period of `years` years |
| `static Period ofMonths(int32 months)` ⚑ | A period of `months` months |
| `static Period ofWeeks(int32 weeks)` | A period of `weeks` weeks (stored as `weeks * 7` days) |
| `static Period ofDays(int32 days)` ⚑ | A period of `days` days |
| `static Period zero()` | The zero period (all fields `0`) |
| `int32 getYears()` | The years field |
| `int32 getMonths()` | The months field |
| `int32 getDays()` | The days field |
| `int64 toTotalMonths()` | Total months (`years * 12 + months`) |
| `boolean isZero()` | True iff every field is zero |
| `boolean isNegative()` | True iff any field is negative |
| `Period plus(Period other)` | Field-wise sum |
| `Period minus(Period other)` | Field-wise difference |
| `Period negated()` | Each field negated |
| `Period normalized()` | Months rolled into years so the month part lands in [-11, 11]; days untouched |
| `int64 hash()` | Content hash mixing the three fields |
| `#String iso()` | ISO-8601 `PnYnMnD` |
| `static boolean operator== (Period a, Period b)` | Value equality — `a == b` |

⚑ = `@EntryPoint`

## See also

- Tour: [TimeDemo](../../../samples/tour/src/main/cajeta/tour/time/TimeDemo.cajeta)
- Source: [`runtime/src/cajeta/time/Period.cajeta`](../../../runtime/src/cajeta/time/Period.cajeta)
- [LocalDate](LocalDate.md) — accepts a `Period` in `plus`; [Duration](Duration.md) — the machine-time sibling
