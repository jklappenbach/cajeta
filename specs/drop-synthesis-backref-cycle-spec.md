# drop-synthesis-backref-cycle — COMPILER SIGSEGV on cyclic drop-function synthesis through interface vtables

## 1. Definition

Found 2026-08-07 implementing cajeta-cloud U3 (memory object-store
driver). The **compiler itself** segfaults (not the compiled program)
when class drop functions form a reference cycle through interface
vtable synthesis:

```cajeta
public final class Store implements ObjectStore {   // iface returns #ObjectWriter
    public #ObjectWriter openWrite(...) { return heap Writer(this, ...); }
}
final class Writer implements ObjectWriter {
    Store store;                                    // back-reference (plain borrow field)
    ...
}
```

Compiling this SIGSEGVs at fault addr 0x8 inside:

```
CajetaClass::synthesizeInterfaceVTables
  → getOrCreateDropFunction → emitDropBodyInline
    → patchVirtualTableDropFn → emitDropBodyInline
      → getOrCreateDropFunction → ...   (recursion, then crash)
```

Synthesizing `Store`'s drop reaches `ObjectWriter`'s vtable, which
needs `Writer`'s drop, whose `Store` field needs `Store`'s
**in-progress** drop — the synthesis recurses into a class whose drop
function is still being emitted and dereferences an incomplete
structure. Plain class-typed borrow fields are enough; no ownership
marker (`#=`) is involved.

## 2. Requirements

- **2.1** Mutually-referencing classes (A holds B, B holds A — directly
  or through an interface each implements) compile; drop synthesis must
  either break cycles (emit a declaration first, patch bodies after) or
  reject the program with a diagnostic — never crash.
- **2.2** A regression pin compiles the store/writer shape above.

## 3. Workaround (in use)

Break the cycle by extracting the shared state into a class that
references **no interface-implementing type**: cajeta-cloud's
`MemState` holds the lock/map/lists/stats, `MemoryObjectStore` owns a
`MemState`, and `MemoryWriter`/`MemoryMultipart` hold the `MemState`
instead of the store — every drop walk terminates.

## 4. Reproduction

cajeta-cloud @ 863164d with `MemoryWriter.st`/`MemoryMultipart.st`
retyped back to `MemoryObjectStore` — `run-tests.sh` crashes the
compiler during the library `--emit=cja` step under v0.17.4.
