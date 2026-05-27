# test-code/

Fixtures consumed by the typing-simulator test harness
(`Cajeta → Test → Type Fixture into Editor`). The harness opens a
fresh editor, types the fixture content character-by-character at
a configurable delay, and records any parse errors that surface at
intermediate states.

## Layout

```
test-code/
├── valid/
│   ├── small/      hand-curated minimal fixtures targeting specific
│   │               features (markdown comments, trailing comments,
│   │               stack/heap allocation, lambdas, streams, …)
│   └── tour/       copied from samples/Tour — real Cajeta code that
│                   exercises the broader surface area
└── invalid/        fixtures with known parse failures; the harness
                    records *where* the parser sees them
```

## Adding a fixture

- **Valid fixtures** should parse cleanly at any intermediate
  prefix-state too — that's the harness's main check.
- **Invalid fixtures** should fail in a predictable place; the
  harness will record the failure location and message so you
  can compare against your expectation. Add a top-line comment
  describing the expected failure, e.g.:

  ```cajeta
  // EXPECT: parse error around `public int32 y;` (missing `;` on line 4)
  package com.example;
  …
  ```

## Tour fixtures

The `valid/tour/*.cajeta` files are unmodified copies of the
samples. Don't edit them in-place — update `samples/Tour/` and
re-copy. (A Gradle task that auto-syncs could land later.)
