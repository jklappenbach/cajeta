# Rust JSON benchmark (cross-language comparison)

serde_json + simd-json vs Cajeta's `Json.parse<T>` binding, on the standard
simdjson corpus (`/tmp/jsonbench/{twitter,citm_catalog,canada}.json`).

```
RUSTFLAGS="-C target-cpu=native" cargo build --release
./target/release/rustjson-bench
```

Methodology mirrors `bench/src/bench/BindBench.cajeta`: peak-of-batches MB/s, and
a copy-only baseline is subtracted for `simd-json` (it mutates its input in
place). The apples-to-apples row for Cajeta's skip-all `parse<BBEmpty>` is
serde_json's `IgnoredAny` (structural skip-all). Results table:
[`docs/specification/codec/json/Json.md`](../../docs/specification/codec/json/Json.md).
