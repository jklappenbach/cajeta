# json-parse-accepts-invalid — `Json.parse` accepts 9 of the 31 JSON_checker fail files (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 9 from the
> `codec/json-conformance` benchmark, which now runs the real JSON_checker
> corpus (pass01–03, fail02–33 without fail18; the set nativejson ships).

## 1. Definition
On v0.46.3 `Json.parse` accepts fail07, 08, 10, 13, 25, 27, 29, 30 and 31:
trailing content after the root value, a leading zero, raw control
characters inside strings, and malformed exponents. Every competitor
(simdjson, yyjson, serde_json, Go encoding/json, Jackson, orjson) rejects all
31 and accepts all 3. The smoke asserts the row as a known `invalid` until the
gap closes and prints "gap closed" when it does.

## 2. Use cases
- **2.1** As a parser user, when the document has trailing content, a leading
  zero, an unescaped control character or a malformed exponent, then
  `Json.parse` rejects it by name.
- **2.2** As the test suite, then every JSON_checker file is a JIT test case
  with its expected verdict, in `test/` beside the existing JSON tests.

## 3. Acceptance
`codec/json-conformance` reports 3/3 and 31/31 and the smoke's known-gap
assertion is removed. The float round-trip half of the same benchmark's
failure (canada's 105,301 float leaves) is `float64-tostring-roundtrip`.
