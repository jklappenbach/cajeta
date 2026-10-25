# keep-owned-primitive-array-local — `this.f = w` with an owned `int64[]` local compiles and dangles (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 2. A gap in
> `field-store-ownership` rule 3 (an owned local stored into a field that
> outlives it must be stored with `#=`, enforced as
> `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`).

## 1. Definition
Measured on v0.46.3 while moving the sort benchmarks: in a class whose
field is `int64[] out`, the statement `this.out = w;` where `w` is an owned
local `int64[]` compiles without a diagnostic. The local is freed at scope
end and the field reads freed memory; `collection/sort-int64` reported its
result `invalid` until the three stores were spelled `#=`, after which every
variant is `ok`. The same shape with a class-typed local is rejected by name.
The rule does not fire for primitive-array locals.

**A stdlib victim** (unit 12, 2026-10-25): `runtime/src/cajeta/nucleo/autograd/Tape.cajeta`
stores its doubled arrays in `grow()` with `this.tag = t2;` and four sibling
stores, so any tape past 32 nodes crashes (17 nodes run, 39 SIGSEGV). With
the five stores spelled `#=` the autograd benchmark passes its check. Fixing
those stores is independent of the compiler check and should happen first;
`nucleo/tape-forward-backward` skips until it does.

## 2. Use cases
- **2.1** As a developer, when I write `this.out = w;` with `w` an owned
  local primitive array that the field outlives, then the compiler reports
  `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE` exactly as it does for a class local.
- **2.2** As the test suite, when the check lands, then a test asserts it
  fires for `int64[]`, `float32[]` and `uint8[]` locals, and a test asserts
  it does not fire for a `#=` store or for a holder that stays local
  (field-store-ownership rule 8).

## 3. Acceptance
The three sort benchmarks in `bench/` keep their `#=` spelling; a `=`
spelling of any of them fails to compile.
