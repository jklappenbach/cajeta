# json-grow-element-uaf — defect (found during cajeta-llm Unit 5)

## 1. Definition

**1.1 Symptom.** A `JsonObject` or `JsonArray` that grows past its 8-entry
starting capacity serves freed memory: lookups return values belonging to
other (later-allocated) objects, or poison. Observed 2026-08-13 as
`JsonObjectHashTests` returning entry 10's value for key `"a7"` on a 20-key
object — the freed `JsonValue`'s block had been recycled. `Headers` and
`QueryParams` carried the same defect past 8 headers / 8 query params.

**1.2 Root cause — grow loops that borrow-copy owned elements, then displace
the source.** Four grow/resize methods copied their elements with plain `=`
stores and then replaced the backing array with a `#=` field store:

- `JsonObject.grow` (`int8[][] keys`, `JsonValue[] values`)
- `JsonArray.grow` (`JsonValue[] data`)
- `Headers.grow` (`String[] keys/values`)
- `QueryParams.grow` (`String[] keys/values`)

A plain element copy is a borrow: the SOURCE array's element tail bits stay
set, so the old array still "owns" every element. The `#=` displacement
release then runs the element-drop walk (title-stores Unit 5, re-enabled on
the premise that "the stdlib is pure-tail now — the 3.2.5 audit's
mixed-owned[] grow loops respelled to forwarding moves") and frees each
element the new array had just borrowed. These four sites were missed by
that audit — `ArrayList.reserve`, `HashMap.resize`, and `Heap` were
respelled; the codec and net stores were not. (`StringBuilder` and
`ByteBuffer` grow primitive `int8[]` buffers, which carry no slot bits and
were never at risk.)

**1.3 Why latent until now.** The defect needs an object past 8 entries AND
allocator reuse of the freed blocks. Configs and manifests in the repo stay
under 8 keys; the first workloads to stress large JSON objects are
cajeta-llm's — a safetensors header (thousands of tensors) and the
tokenizer vocab (~100k keys). The 5.2.4 hash-index tests hit it on their
first run.

## 2. Fix

**2.1** All four sites respelled to forwarding moves (`dst[i] #= src[i]`),
the same discipline as `ArrayList.reserve` — the take clears the source's
tail bit, so the displacement walk finds only slots the old array genuinely
still owns (after a full move-out: none).

**2.2** Pinned by `test/parser/JsonObjectHashTests.cpp`: the two JsonObject
tests force growth to 20–30 entries with interleaved allocations, and
`arrayGrowthKeepsElementsAlive` forces two JsonArray grows (40 elements).
`Headers`/`QueryParams` ride the existing net suites plus the shared
one-line fix shape.

## 3. Checklist for the next audit

Any `this.<field> #= <freshArray>` displacement whose copy loop reads
class-reference or array elements must move them (`#=`), never `=`. A grep
for `#= dst|#= grown` over `runtime/src` enumerates the sites in minutes —
run it whenever the element-drop walk's preconditions change.

## 4. Status

Fixed on `cajeta-llama/unit-5-stdlib` with the 5.2.4 hash index (the tests
that exposed it).
