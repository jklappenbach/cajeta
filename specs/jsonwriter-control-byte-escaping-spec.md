# jsonwriter-control-byte-escaping — defect

Found 2026-08-07 implementing `cajeta-llm` Unit 12 planning (filed per
cajeta-llm spec 13.7/13.8; kept as plan item 12.1.2 until this spec was
authored 2026-08-14).

## 1. Defect

`JsonWriter.writeStringRaw` (`runtime/src/cajeta/codec/json/JsonWriter.cajeta:338-355`)
escapes only `"` (0x22) and `\` (0x5C). Any byte in 0x00–0x1F reaches the
output verbatim, and RFC 8259 §7 forbids unescaped control characters in a
JSON string — so a written string containing a newline, tab, carriage
return, NUL, or any other control byte produces **invalid JSON** that
`JsonReader` itself, and every conforming external parser, may reject or
misparse.

Repro sketch:

```cajeta
JsonWriter w = heap JsonWriter();
w.writeString("line1\nline2");   // emits a raw 0x0A inside the quotes
```

The emitted document contains a literal LF between the quotes instead of
`\n`. Round-tripping through a conforming reader fails; feeding the output
to any external consumer (HTTP APIs, jq, python json) fails.

Reads are unaffected: `JsonReader` correctly *accepts* `\n`, `\t`, `\uXXXX`
et al. The defect is write-side only. It is distinct from the codec-wide
verbatim-escape *read* design (`JsonValue.cajeta:125`,
`JsonIndex.cajeta:28,152,355`), which is deliberate and not a defect
(cajeta-llm spec 13.7).

## 2. Requirements

- **2.1** When a string containing a byte in 0x00–0x1F is written, the
  output escapes it: `\b` (0x08), `\t` (0x09), `\n` (0x0A), `\f` (0x0C),
  `\r` (0x0D) as their two-character forms, every other control byte as
  `\u00XX`.
- **2.2** When a string contains `"` or `\`, the existing escapes are
  unchanged.
- **2.3** When a string contains no byte requiring escape, output is
  byte-for-byte identical to today — the fast path stays fast and existing
  golden outputs without control bytes do not change.
- **2.4** When escaped output is read back by `JsonReader`, the decoded
  string round-trips to the original bytes.
- **2.5** When any existing stdlib JSON consumer writes documents without
  control bytes (the overwhelmingly common case), its output is unchanged —
  the fix cannot alter serialization of clean strings.

## 3. Scope

`writeStringRaw` and any sibling raw-string emitters in `JsonWriter`. No
reader changes; no `currentBytes()` changes; no API additions. Bytes ≥ 0x80
(UTF-8 sequences) continue to pass through verbatim — JSON permits raw
UTF-8; only 0x00–0x1F is at issue.
