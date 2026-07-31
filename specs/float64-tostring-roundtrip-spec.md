# float64 shortest-round-trip rendering — spec (defect)

Found by the tour-quality logging review (findings/logging.md, Defects).

## 1. Definition

- 1.1 stdlib float64-to-string is not shortest-round-trip: `0.987` renders as
  `0.986999` via `cajeta.codec.json.JsonWriter.writeNumber(float64)`, which
  `Float64.toString` shares (observed in cajeta-logging's logfmt/text output).
- 1.2 Expected: shortest decimal string that round-trips to the same bits
  (Ryū / Grisu class algorithm), as every modern runtime provides.

## 2. Use cases

- 2.1 As a developer, when I print or log any float64, then the rendered
  string parses back to the identical bit pattern and is the shortest such
  string (`0.987`, not `0.986999`).
- 2.2 As a JSON producer, when I serialize float64 fields, then values survive
  serialize → parse round trips bit-exactly.

## 3. Acceptance

- 3.1 Round-trip property test over representative values (decimals, powers
  of two, subnormals, extremes) plus the pinned `0.987` case; logging and
  codec suites green.
