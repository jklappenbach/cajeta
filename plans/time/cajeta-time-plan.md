# `cajeta.time` — Standard Library Implementation Plan (TDD)

> Status: **v1 implemented on `feature/time`** (Phases 0–5). Shipped: Comparable,
> DateTimeException/DateTimeParseException, Clock, Duration, Instant, LocalTime,
> LocalDate, LocalDateTime, Period, ZoneOffset, ZonedDateTime — all with JIT
> tests under `test/time/`. Deferred (scoped below): Phase 6 region `ZoneId` +
> tz database, Phase 7 `DateTimeFormatter` pattern engine + parsing. Resolved
> decisions (§8): created `cajeta.lang.Comparable<T>`; no `equals` (structural
> `operator==` only); time-specific exceptions on the recoverable branch;
> `@Native` clock binding (no compiler change); `iso()` instead of a `toString`
> override; day-of-week as ISO int (1=Mon..7=Sun); Phases 6–7 deferred.
> Implementation constraints learned: value types use a single canonical field
> and inline `return stack` literals only — see the memory note "cajeta value-type rules".
>
> Original status: **Plan / design — awaiting review.**
> Scope: implement the `cajeta.time` package, a `java.time` (JSR-310)-style
> date/time library, in cajeta source (`runtime/src/cajeta/time/`) plus the
> small native clock surface it needs.
> Authoritative spec: **`cajeta-docs/stdlib/Time.md`**. Feature tracker:
> **`Features.md` § "Stdlib — `cajeta.time`"** (S-501 … S-507).
> Today only `Duration.cajeta` (a minimal nanos wrapper) exists; everything
> else is unimplemented.

---

## 0. Goals & parity target

Build an immutable, value-semantics date/time library modeled on `java.time`,
pared to cajeta's needs:

- **Clock** — static "what time is it" surface (monotonic ns, wall-clock ms, `now()`).
- **Instant** — a moment on the UTC timeline, ns precision.
- **Duration** — a time-based amount (ns), already stubbed; to be completed.
- **Period** — a calendar-based amount (years/months/days).
- **LocalDate / LocalTime / LocalDateTime** — zone-naive date/time.
- **ZoneOffset / ZoneId / ZonedDateTime** — fixed offsets first, tz database later.
- **DateTimeFormatter** — ISO-8601 first, full pattern letters later.

**Non-goals for v1** (explicitly out of scope, revisit later):
`Chronology`/non-ISO calendars, `OffsetDateTime`/`OffsetTime` (subsumed by
`ZonedDateTime` + `ZoneOffset` initially), `Year`/`YearMonth`/`MonthDay`,
`temporal.TemporalAdjusters`, leap-second tables (we follow java.time's
UTC-SLS "ignore leap seconds" convention), localized/non-English formatting.

---

## 1. Methodology (binding)

Test-first, matching the repo convention (there is **no `.cajeta` test runner**;
stdlib is exercised by C++ tests that compile cajeta and assert on behavior —
see `test/expression/StringMethodsTests.cpp`, `test/collections/*`). For each
type below:

1. Write the cajeta class skeleton (fields + signatures, bodies stubbed).
2. Add a C++ test file under `test/time/` (new dir) that compiles a small
   cajeta `main()` using the type and asserts results (mirrors how
   `StringMethodsTests` drives `cajeta.lang.String`).
3. Implement until green. Run via `./run_tests.sh` (compiles + runs the C++ suite).

The CMake stdlib glob already lists `cajeta/time` (`src/CMakeLists.txt`
`CAJETA_STDLIB_DIRS`), so new `.cajeta` files in that dir are auto-compiled —
**no per-file registration**. A new `test/time/` dir will need a `CMakeLists`/
`add_subdirectory` entry alongside the other `test/*` suites.

---

## 2. Cross-cutting design decisions

These apply to every type. **Several need your sign-off (§8).**

### 2.1 Value semantics
Every type is an **immutable** class with `final`-style fields set only in the
constructor (cajeta has no setters idiom here). Following `cajeta.lang.String`
(`String.cajeta:107`), value equality is obtained by **overriding `hash()`** so
the inherited `Object.operator==` (compares `a.hash() == b.hash()`) gains value
semantics. We will override `hash()` on all value types with a FNV-style mix of
the fields.

> ⚠️ `Object.operator==` compares **only** hashes, so equality is as strong as
> the hash is collision-free. For 64-bit-domain types (`Instant`, `Duration`)
> two distinct values can share a hash. **Decision needed (§8-A):** keep the
> hash-only equality (cheap, matches `String`) or add a real `boolean
> equals(T)` method per type and have callers prefer it. Recommendation: add a
> typed `equals(T)` on each type AND override `hash()`; document that `==`
> is hash-based like the rest of the stdlib.

### 2.2 Ordering / comparison
java.time types are `Comparable`. **cajeta has no `Comparable` interface today.**
Two options (**Decision needed §8-B**):
- (Recommended) Add `cajeta.lang.Comparable<T>` (a one-method templated
  interface: `int32 compareTo(T other)`), and have each time type implement it.
  Templated-interface *declaration* already works (Features L-26); impl-side
  vtable instantiation is "future work", so for v1 we may implement
  `compareTo` as a **plain method** without formally implementing the interface
  until vtables land. Plan: ship `int32 compareTo(T)` as a plain instance
  method now; retrofit the interface when L-26 vtables are ready.
- Also overload comparison operators where ergonomic: `operator<`, `operator<=`,
  etc. are `public static boolean` (per `OperatorOverloading.md`). We'll add at
  least `operator<` / `operator>` for the ordered types.

### 2.3 Allocation
Small fixed-size value types return **by stack value** (`return stack T(...)`),
exactly like `Duration.ofNanos` (`Duration.cajeta:26`). No heap, no `#`-transfer
for the value types. Types that own a heap array (`ZoneId` holding its id bytes,
`DateTimeFormatter` holding a compiled pattern) use `heap` + `#`-transfer like
`Path.of` (`io/file/Path.cajeta:53`).

### 2.4 Exceptions
Add to `cajeta.time` (or `cajeta.error`? **Decision §8-C** — recommend keeping
them in `cajeta.time` next to their users):
```
DateTimeException        extends RecoverableException   // invalid field values, etc.
DateTimeParseException   extends DateTimeException       // + int64 errorIndex
```
Follow the `JsonParseException` pattern (`codec/json/JsonParseException.cajeta`):
direct field assignment in the ctor (no `super()` yet), `throw heap
DateTimeException("…")`.

### 2.5 Integer types & literals
All fields are `int32`/`int64` (signed). **Underscore digit literals are NOT
used in real code** (only in comments) — use plain digits (e.g. `1000000000`),
confirm-or-fix if the lexer rejects large literals. No floating point in the
core (nanos are integers). Casts are explicit (`(int64) x`).

### 2.6 Date arithmetic = pure cajeta (no native)
All calendar math (days-from-civil, civil-from-days, day-of-week, leap-year,
length-of-month) is **pure int64 arithmetic** using Howard Hinnant's well-known
`date` algorithms — no native code, fully testable in cajeta. Only *reading the
current time* and *tz-database lookups* touch native.

### 2.7 String building dependency
`toString()` and `DateTimeFormatter` need to build strings from integers
(zero-padded fields). **There is no `StringBuilder` and no confirmed
int→String formatter in the stdlib yet** (concat-stringify via `+` is mentioned
in `Primitives.md` but unverified for our needs). **Dependency/risk (§9):** we
likely need a tiny internal helper (build a `int8[]`, write ASCII digits, wrap
in `heap String(#bytes, len)`). Plan to add a private `cajeta.time` formatting
helper rather than block on a general StringBuilder.

---

## 3. Native surface (the only C/compiler work)

Minimal. The C functions **already exist** in `runtime/native/cajeta_runtime.c`:
- `__cajeta_currentTimeNanos()` → `clock_gettime(CLOCK_MONOTONIC)` (≈line 1795)
- `__cajeta_currentTimeMillis()` → `clock_gettime(CLOCK_REALTIME)` (≈line 3825)

`Clock` binds to them with the general `@Native("symbol")` mechanism used by
`Object.hash` (`lang/Object.cajeta:76`):
```cajeta
public final class Clock {
    @Native("__cajeta_currentTimeNanos")  public static int64 nanoTime();
    @Native("__cajeta_currentTimeMillis") public static int64 millisTime();
    public static Instant now() {
        int64 ms = Clock.millisTime();
        return Instant.ofEpochMilli(ms);
    }
}
```

**Investigation item (§8-D):** confirm whether `@Native` on a `static` method
auto-routes by symbol name (so no compiler edit is needed) or whether
namespaced intrinsics like `Cajeta.currentTimeNanos` are special-cased in
`src/cajeta/asn/expression/MethodCallExpression.cpp` and a similar hook is
required. If `@Native` is general (most likely, given `Object.hash`), **zero
compiler changes** are needed. tz-database access (Phase 6) may add one native
function (`__cajeta_tz_lookup`) — deferred.

---

## 4. Type-by-type design

Field layouts and the headline API per type. (Full method lists tracked in the
phase checklists, §6.)

### 4.1 `Duration` (complete the stub) — S-503
Fields: `int64 nanos` (exists). Add: `ofHours/ofDays`; accessors
`toMillis/toSeconds/toMinutes/toHours/toDays`, `secondsPart`/`nanosPart`;
arithmetic `plus/minus/multipliedBy/dividedBy/negated/abs`; predicates
`isZero/isNegative`; `compareTo`, `hash`, `toString` (ISO-8601 `PnDTnHnMnS`).
Operators: `operator+`, `operator-` (static, `Duration×Duration`).

### 4.2 `Instant` — S-502
Fields: `int64 epochSecond`, `int32 nano` (0…999,999,999), normalized in ctor.
API: `now()` (via Clock), `ofEpochSecond(s)`, `ofEpochSecond(s, nanoAdj)`,
`ofEpochMilli(ms)`; `getEpochSecond/getNano/toEpochMilli`; `plus(Duration)`/
`minus(Duration)`/`plusSeconds/plusMillis/plusNanos`; `Instant.between(a,b) →
Duration`; `isBefore/isAfter`, `compareTo`, `hash`, `toString` (ISO-8601 UTC
`…Z`, via `LocalDateTime` of the UTC projection). `atZone(ZoneId) →
ZonedDateTime` (Phase 5).

### 4.3 `LocalDate` — S-505
Fields: `int32 year`, `int32 month` (1–12), `int32 day` (1–31). Validated in
ctor (throw `DateTimeException` on out-of-range, month-length aware).
API: `of(y,m,d)`, `ofEpochDay(int64)`, `now(ZoneId)` (Phase 5; `now()` uses
system zone later — for v1 a `nowUtc()` helper); `getYear/Month/DayOfMonth`,
`getDayOfWeek` (0–6 or an enum — see §8-E), `getDayOfYear`, `lengthOfMonth`,
`isLeapYear`; `plusDays/Months/Years`, `minus…`; `toEpochDay() → int64`;
`compareTo/isBefore/isAfter`, `hash`, `toString` (`yyyy-MM-dd`). Epoch-day
conversions use the civil↔days algorithms (§2.6).

### 4.4 `LocalTime` — S-505
Fields: `int32 hour` (0–23), `int32 minute` (0–59), `int32 second` (0–59),
`int32 nano` (0–999,999,999). API: `of(h,m)`, `of(h,m,s)`, `of(h,m,s,n)`,
`ofNanoOfDay(int64)`, `ofSecondOfDay`; getters; `toNanoOfDay/toSecondOfDay`;
`plusHours/Minutes/Seconds/Nanos` (wrap mod 24h, **no** day-overflow carry in
v1 — document); `compareTo`, `hash`, `toString` (`HH:mm`, `HH:mm:ss`, or
`HH:mm:ss.nnnnnnnnn` minimal form). Constants `MIDNIGHT`, `NOON`, `MIN`, `MAX`.

### 4.5 `LocalDateTime` — S-505
Fields: `LocalDate date`, `LocalTime time` (composition). API: `of(date,time)`,
`of(y,mo,d,h,mi[,s[,n]])`, `ofEpochSecond(s, nano, ZoneOffset)`; `getDate/getTime`
+ delegating getters; `plus…`/`minus…` with **correct day carry** between time
and date; `compareTo/isBefore/isAfter`, `hash`, `toString` (`yyyy-MM-ddTHH:mm…`);
`atZone(ZoneId) → ZonedDateTime` (Phase 5); `toInstant(ZoneOffset) → Instant`.

### 4.6 `ZoneOffset` — S-506 (fixed offsets, no tzdata)
Fields: `int32 totalSeconds` (−18h…+18h). API: `ofHours/ofHoursMinutes/
ofTotalSeconds`, `UTC` constant, `of("±HH:mm")` parse; `getTotalSeconds`,
`getId` (`Z` / `±HH:mm`), `compareTo`, `hash`, `toString`. **`ZoneOffset` is the
v1 backbone** — `ZonedDateTime` can be built on offsets before the tz DB lands.

### 4.7 `Period` — S-504
Fields: `int32 years`, `int32 months`, `int32 days`. API: `of/ofYears/ofMonths/
ofDays`, `ZERO`; getters; `plus/minus/normalized`; `LocalDate.plus(Period)`
integration; `hash`, `toString` (ISO `PnYnMnD`). No nanos (calendar amount).

### 4.8 `ZoneId` + `ZonedDateTime` — S-506 (Phase 5/6)
- **v1 (Phase 5):** `ZoneId` is effectively a `ZoneOffset` (region IDs deferred).
  `ZonedDateTime` = `LocalDateTime` + `ZoneOffset`, round-trippable to `Instant`.
- **Phase 6 (deferred / its own session):** region `ZoneId.of("America/Los_Angeles")`
  backed by `/usr/share/zoneinfo` (read-from-disk, one native lookup
  `__cajeta_tz_offset(zoneNameBytes, epochSecond) → int32 offsetSeconds`), with
  an embedded UTC + fixed-offset fallback for static builds (per spec open
  question). DST transitions handled by the offset-at-instant lookup.

### 4.9 `DateTimeFormatter` — S-507 (Phase 7, deferred)
- **v1:** ISO-8601 only, delivered as the `toString()` of each type + static
  `parseLocalDate/parseLocalTime/parseInstant` helpers (strict ISO). No general
  pattern engine yet.
- **Phase 7 (deferred):** pattern letters (`yyyy-MM-dd HH:mm`, etc.), `ofPattern`,
  `format(temporal)`, `parse`. Depends on the string-building helper (§2.7) and
  a decision on the temporal-field abstraction.

---

## 5. Algorithms reference (for the date math)

- **days_from_civil(y,m,d) → epochDay** and **civil_from_days(epochDay) →
  (y,m,d)**: Hinnant's algorithms (branch-free int64; valid for the full
  proleptic Gregorian range). Day 0 = 1970-01-01.
- **day_of_week(epochDay)**: `((epochDay % 7) + 11) % 7` style (define the
  0=Sun/Mon convention in §8-E).
- **is_leap(y)**: `y%4==0 && (y%100!=0 || y%400==0)`.
- **last_day_of_month(y,m)**: table + Feb leap adjust.
These are pure functions — implement once (private statics on `LocalDate`) and
unit-test exhaustively against known dates (epoch, Y2K, leap days, pre-1970
negatives).

---

## 6. Phased delivery (each phase = test-first, ships green)

- [ ] **Phase 0 — scaffolding.** `test/time/` C++ harness + CMake wiring;
      `DateTimeException` / `DateTimeParseException`; the private int→ASCII
      string helper (§2.7). Confirm `@Native` binding path (§3 / §8-D).
- [ ] **Phase 1 — `Clock` + `Instant` + finish `Duration`.** (S-501, S-502,
      S-503.) Native clock binding; `Instant.now()`; `Instant ↔ epochMilli`;
      `Instant.between`; Duration arithmetic + ISO toString. Tests: clock
      monotonicity, epoch round-trips, duration algebra.
- [ ] **Phase 2 — `LocalDate`.** (S-505a.) Civil↔days algorithms + exhaustive
      conversion tests; validation throws; `plus/minus`; `toString`.
- [ ] **Phase 3 — `LocalTime` + `LocalDateTime`.** (S-505b/c.) Nano-of-day math;
      composition + day-carry arithmetic; `toInstant(ZoneOffset)`.
- [ ] **Phase 4 — `Period`** (S-504) + `LocalDate.plus(Period)`.
- [ ] **Phase 5 — `ZoneOffset` + offset-based `ZonedDateTime`.** (S-506a.)
      `Instant ↔ ZonedDateTime` round-trip via fixed offset.
- [x] **Phase 6 — region `ZoneId` + tz database.** (S-506b.) **Done.** Native
      `__cajeta_tz_offset` parses TZif v1/v2 from `/usr/share/zoneinfo` and returns
      the DST-aware offset at an epoch second; `ZoneId.of(name).offsetAt(instant)`
      → `ZoneOffset`, `ZoneId.resolve(instant)` → `ZonedDateTime`. UTC/GMT/Z
      fast-path works without the filesystem (static builds); unknown zones throw.
      Tests: `test/time/ZoneIdTests.cpp` (DST straddle, half-hour offset, throw).
- [x] **Phase 7 — `DateTimeFormatter`.** (S-507, partial.) **Done:** strftime
      `ofPattern("%Y-%m-%d %H:%M:%S")` (codes Y/y/m/d/H/I/M/S/p/j/a/A/b/B/z/Z/f/L/%/n/t)
      + `FormatStyle` standards (ISO_LOCAL_DATE[_TIME], ISO_OFFSET/INSTANT,
      BASIC_ISO_DATE, RFC_1123, US/EURO_DATE, SQL_TIMESTAMP) over a `DateTimeFields`
      render engine. Formatter is **immutable** (stores the pattern string, parses
      at `format()`). Tests: `test/time/DateTimeFormatterBuilderTests.cpp` (11).
      **Deferred:** the fluent step-builder decorator — cajeta's codegen for
      self-returning fluent methods (`return this`) on a heap object is unsound
      (value-poisons the class, breaks the stdlib JIT verify); `ofPattern` covers
      the same need. Parsing (text → temporal) also deferred.

      > **Deferred — chained-decorator builder.** If you want the chained
      > decorator specifically, it's a compiler fix, not a library one — the
      > fluent-method this-by-value codegen needs to be made sound. I'm happy to
      > dig into that in src/cajeta/ as a separate effort, or wire the builder up
      > the moment that lands. Parsing (text → temporal) is also still open.
      >
      > **Compiler dependency:** make instance methods that `return this` (and,
      > more generally, methods returning their own class type) pass and return
      > the receiver **by reference** for heap/`#`-constructed instances, instead
      > of by value. Today a self-returning method receives `this` by value, so
      > (a) field mutations don't persist to the caller's variable, and (b)
      > invoking such a method on a `heap`-built instance — or passing that class
      > as a parameter — emits LLVM that fails the JIT verifier
      > (`Call parameter type does not match function signature`,
      > `SExt only operates on integer`), which takes down the whole embedded
      > stdlib compile. Likely fix sites: the return-ABI / storage-class
      > resolution in `src/cajeta/method/Method.cpp` (the same pass that decides
      > value-vs-reference returns; see `Method.cpp:~849` `returnsStackValue`) and
      > the value-return lowering in `src/cajeta/asn/Statement.cpp` (the
      > `FRESH_RETURN_NEEDS_TRANSFER` / by-value struct-return path). Once
      > self-returns are reference-stable, restore `DateTimeFormatBuilder`
      > (a heap object with an `int8[]` pattern buffer and `return this` step
      > methods) + `DateTimeFormatter.builder()`.

Phases 1–5 are the committed v1 of this branch; 6–7 are scoped here but
explicitly deferred (flagged so we don't silently drop them).

---

## 7. Testing plan

Per-type C++ suites under `test/time/` (compile a cajeta `main`, assert):
- `DurationTests`, `InstantTests`, `LocalDateTests` (incl. a table of
  ~30 known civil/epoch-day pairs spanning negatives), `LocalTimeTests`,
  `LocalDateTimeTests`, `PeriodTests`, `ZoneOffsetTests`, `ZonedDateTimeTests`.
- Edge cases: epoch boundary, pre-1970 negatives, leap days (2000-02-29,
  1900-02-28), month-length validation throws, Duration overflow near ±292y,
  nano normalization, offset parse `Z`/`+05:30`/`-08:00`.
- Update `Features.md` S-50x rows from **designed → shipped** as each lands,
  and (separately) regenerate cajetadoc so the new package documents itself.

---

## 8. Decisions needed before I start (please review)

- **§8-A — Equality model.** Add a typed `boolean equals(T)` per type *and*
  override `hash()` (recommended), or rely on hash-only `==` like `String`?
- **§8-B — `Comparable`.** OK to ship `int32 compareTo(T)` as a plain method now
  and add a real `cajeta.lang.Comparable<T>` interface later (when templated-
  interface vtables land, L-26)? Or hold ordering until the interface exists?
- **§8-C — Exception home.** Put `DateTimeException`/`DateTimeParseException` in
  `cajeta.time` (recommended) or `cajeta.error`?
- **§8-D — Native binding.** Confirm `@Native("__cajeta_currentTimeNanos")` on a
  static method routes with **no** compiler change (expected, matching
  `Object.hash`). If it doesn't, Phase 0 adds a one-line hook in
  `MethodCallExpression.cpp`.
- **§8-E — Day-of-week & enums.** Does cajeta have usable `enum` for
  `DayOfWeek`/`Month`, or should these be `int32` constants for v1? (Pick the
  0=Mon vs 0=Sun convention too.)
- **§8-F — Scope cut.** Confirm Phases 6 (tz database) and 7 (pattern formatter)
  are acceptable to **defer** out of this branch, leaving fixed-offset
  `ZonedDateTime` + ISO-8601 strings as the v1 surface.

## 9. Risks & dependencies

- **String building (§2.7):** no `StringBuilder`/int-format confirmed. Mitigation:
  a small private byte-builder in `cajeta.time`; flagged so it isn't a surprise.
- **Hash-based `==` (§2.1):** weaker than structural equality; mitigated by typed
  `equals` if §8-A approved.
- **Templated-interface vtables (L-26):** blocks a *formal* `Comparable`;
  mitigated by plain `compareTo` (§8-B).
- **tz database portability:** static builds lack `/usr/share/zoneinfo`;
  mitigated by UTC + fixed-offset fallback (Phase 6, deferred).
- **`super()` not available:** exception ctors assign fields directly (matches
  existing `cajeta.error` classes) — no blocker, just noted.

## 10. Out of scope (v1)
Non-ISO chronologies, `OffsetDateTime`/`OffsetTime`, `Year`/`YearMonth`/
`MonthDay`, `TemporalAdjusters`, localized formatting, leap seconds.
