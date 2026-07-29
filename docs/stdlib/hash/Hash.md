# Hash

`cajeta.hash.Hash` — static utility surface for the runtime hash primitives:
pointer-based identity hashing (for identity-keyed tables and observer
registries, where *same heap object* is the equality notion), manual
combination of two field hashes for hand-written `hash()` overrides, and
access to the per-process random seed so external callers can align with the
compiler-synthesized `Object.hash()`. All entry points are static — there is
nothing to construct.

```cajeta
Object obj = heap Object();
int64 id = Hash.identity(obj);        // same-heap-object identity
int64 seed = Hash.processSeed();      // e.g. to replay a hash-table snapshot
int64 mixed = Hash.combine(id, seed);
```

## Methods

| Signature | |
|---|---|
| `static int64 identity(Object obj)` ⚑ | Pointer-based identity hash |
| `static int64 combine(int64 a, int64 b)` ⚑ | Combine two 64-bit hash values into one with good distribution |
| `static int64 processSeed()` ⚑ | The process-wide random seed |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/hash/Hash.cajeta`](../../../runtime/src/cajeta/hash/Hash.cajeta)
- [DefaultHasher](DefaultHasher.md) — the process-seeded hasher behind `Object.hash()`
- [XXHash3](XXHash3.md) — the algorithm backing it
