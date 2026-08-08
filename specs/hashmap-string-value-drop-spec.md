# hashmap-string-value-drop — HashMap with a String VALUE type double-frees on map drop

## 1. Definition

Found 2026-08-07 implementing cajeta-cloud U2 (`Capabilities`, backed by
`HashMap<String, String>`). A map whose **value** type is `String`
double-frees a stored string when the map itself drops — one `put` is
enough:

```cajeta
HashMap<String, String> m = heap HashMap<String, String>();
String k = "" + "alpha";
m.put(#k, "" + "beta");
// scope exit: SIGSEGV — drop chain shows the stored String
// (alloc at the `String k` line) with active=0 being dropped again
// while the map (active=1) drops
```

Bounding probes:
- `HashMap<String, Int64>` with the identical shape (`put(#k,
  Int64.of(...))`) — clean. This is the only value-type family the
  shipped ecosystem exercises (Vectorizer, DocSet, Subword,
  Interactions), which is why the defect never surfaced.
- An empty `HashMap<String, String>` (no put) — clean.
- The arg spelling is irrelevant: moved locals (`#k, #v`), a bare
  fresh temp (`"" + s`), and an explicit `#(expr)` wrapper all crash
  identically once a String value is stored.

Likely mechanism (from `HashMap.remove`'s own comments): the String
**field-store emitter old-drops the previous field value
unconditionally** — remove() already documents having to clear the key
slot from a stack-sourced zero to dodge this ("the 6.2.6b DnsCache
eviction SIGSEGV"). The `put` path's String **val**-slot store appears
to hit the same emitter without the equivalent guard, releasing a
stored string that the map's drop walk then frees a second time.

## 2. Requirements

- **2.1** `HashMap<K, String>` stores, gets, overwrites, removes, and
  drops without double-free for String and class K.
- **2.2** A regression pin puts at least two `String` values (fresh
  key and overwrite of an existing key), reads them back, and lets the
  map drop.

## 3. Workaround (in use)

Avoid String-valued HashMaps entirely: cajeta-cloud `Capabilities`
keeps parallel `ArrayList<String>` lists (names + caveat notes) with a
linear `indexOf` scan — adapters declare fewer than a dozen
capabilities, so the scan is free.

## 4. Reproduction

The 11-line program above as a standalone `--emit=exe` run under
v0.17.4; also cajeta-cloud @ 863164d^ (the pre-workaround
`Capabilities`) — `CapabilityTest::supportsAnswersDefinitively`
SIGSEGVs on its first `declare`.
