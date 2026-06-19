# JSON codec benchmarks

Same-machine throughput comparison of the Cajeta JSON reader against Jackson and
Gson, on the canonical [nativejson-benchmark](https://github.com/miloyip/nativejson-benchmark)
corpus. Workload: **streaming tokenization** (pull every token to end-of-document,
numbers read lazily, no DOM) — the apples-to-apples operation across all three.

Results and analysis live in [`docs/specification/codec/json/Json.md`](../docs/specification/codec/json/Json.md)
§ "v1 — measured". Headline: Cajeta tokenization is Gson-class (within ~2×) and
~3–4× behind Jackson; the correctness gaps found here (no float parsing in the
value tree, Tier-3 DOM exhausting the live-allocation set on ~1.7 MB, `JsonReader`
aliasing its input buffer) outrank the speed gap.

## Reproduce

```sh
# 1. datasets
mkdir -p /tmp/jsonbench
for f in twitter citm_catalog canada; do
  curl -fsSL "https://raw.githubusercontent.com/miloyip/nativejson-benchmark/master/data/$f.json" \
    -o /tmp/jsonbench/$f.json
done

# 2. Cajeta (native, --release). Note: only citm is integer-only; tokenization
#    runs on all three because it doesn't convert numbers.
cajeta --emit=exe --release -o /tmp/jsonbench/cajbench bench.JsonBench.run bench/src /tmp/jsonbench/arch
/tmp/jsonbench/cajbench

# 3. Java (Jackson + Gson), same files
mkdir -p /tmp/jsonbench/lib
curl -fsSL "https://repo1.maven.org/maven2/com/fasterxml/jackson/core/jackson-core/2.18.2/jackson-core-2.18.2.jar" -o /tmp/jsonbench/lib/jackson-core.jar
curl -fsSL "https://repo1.maven.org/maven2/com/google/code/gson/gson/2.11.0/gson-2.11.0.jar" -o /tmp/jsonbench/lib/gson.jar
javac -cp "/tmp/jsonbench/lib/jackson-core.jar:/tmp/jsonbench/lib/gson.jar" -d /tmp/jsonbench bench/java/Bench.java
java -cp "/tmp/jsonbench/lib/jackson-core.jar:/tmp/jsonbench/lib/gson.jar:/tmp/jsonbench" Bench
```

## Methodology notes

- **Cajeta** is native AOT (`--release`, no JIT warmup); each iteration reads a
  fresh buffer (the reader aliases its input, so buffers can't be shared) and a
  read-only baseline is subtracted to isolate tokenization.
- **Java** gets generous JIT warmup (100 iters) then 200 measured; bytes are read
  once and parsed in-memory. Jackson parses `byte[]`; Gson reads a UTF-8 `Reader`.
- All three emit identical token counts per file (twitter 29 573, citm 85 035,
  canada 223 236) — a structural correctness cross-check.
- `JsonBench.cajeta` was built with the cvm `0.7.0` toolchain; the reader code is
  unchanged on `main`/`feature/json-schema`.
