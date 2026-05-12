# Cajeta Memory Model — Specification v1

## Goals

- **Single-owner heap.** Every heap-allocated value has exactly one owning reference at any time.
- **Implicit borrow, explicit transfer.** `=` borrows by default; the `#` operator transfers ownership.
- **Static safety, no user-visible annotations.** All borrow/lifetime errors are compile-time. Scope-based inference; no lifetime syntax for users to write.
- **Zero runtime cost in release.** Heap blocks are plain memory; drops happen at owner scope-end. A debug build flag adds runtime verification for testing the static checker.

## Non-goals (v1)

- Multi-threading safety (deferred — needs Send/Sync-style protocol).
- FFI safety beyond signature-trust.
- `unsafe` escape hatch.
- Reflection / dynamic-dispatch borrow analysis (deferred until virtuals land).

---

## Operator: `#`

Single token. Appears in three positions:

| Position | Meaning |
|----------|---------|
| `#expr` (value position) | Transfer ownership *from* expr. After this, expr is moved; static error to use it again. |
| `#T` (parameter type) | Parameter accepts ownership. Caller must pass `#x` (or auto-promoted fresh `new`). |
| `#T` (return type) | Function transfers ownership to caller. |

**Redundant `#` is a static error.** If a function's return type is `#T`, the callsite assignment `x = f()` already transfers; writing `x = #f()` is rejected. One transfer point per expression.

---

## Borrow / transfer rules

| Operation | No `#` | With `#` |
|-----------|--------|----------|
| `a = b` | a borrows b | a takes ownership; b is moved |
| `f(x)` | f borrows x | f takes ownership; x moved |
| `return x` | caller borrows x (signature: `T f(...)`) | caller takes ownership (signature: `#T f(...)`) |

**Auto-promotion for fresh `new`.** An anonymous `new T(...)` expression in transfer position promotes implicitly. `p.field = new T()` and `return new T()` (in a `#T` function) work without explicit `#`. The temporary is an unnamed owner with no prior identity, so promotion has no use-after-move risk.

---

## Static analysis rules

### Lifetime inference (intra-function)

The compiler tracks each owner's declaration site and scope-end. For each borrow, it tracks the source's lifetime. A borrow that may outlive its source is a compile error at the use site.

### Path-based borrow tracking

Borrows track their *path* from the named root, not just the variable. `String n = person.address.city` records `n`'s root as `person` with path `address.city`. Reassigning any link along the path (`person.address = #x`) drops everything derived from that link; subsequent use of `n` is a static error.

### Function signatures and elision

Signature shape determines the rule. No annotations beyond the `#` marker.

- **Method:** `T foo()` returns a borrow tied to `this`. `#T foo()` returns ownership.
- **Free function, single parameter:** `T foo(P p)` returns a borrow tied to `p`. `#T foo(P p)` returns ownership.
- **Free function, multiple parameters:** borrow-returns are **forbidden**. Multi-parameter functions must return `#T` (or a primitive value type). The caller cannot disambiguate which parameter a borrow inherits from without annotations, so we forbid the case rather than re-introduce lifetime syntax.

The compiler verifies each function body conforms to its signature: a `T foo(P p)` whose body returns a borrow rooted in something other than `p` is rejected at the definition site.

### Anonymous-owner error

A chained access whose root is an unnamed temporary, where any intermediate produces a borrow, is a static error:

```
String name = factory.makeUser().getName();
//             └ anon User (drops at end of expression)
//                             └ borrow into it — would outlive owner
//                               STATIC ERROR
```

Fix: bind the intermediate.

```
User u = factory.makeUser();
String name = u.getName();
```

### Alias-mutation

A live borrow into a thing blocks mutation of (or through) that thing's path. Iteration is recognized as a borrow construct:

```
for (String s : list) {
    list.add(#thing);   // STATIC ERROR — list has live iterator borrow
}
```

### Drop order

LIFO within a scope; inner scopes drop before outer. A borrow declared before its source is a static error (source's scope ends first; borrow would dangle).

---

## Fields

- **Fields are always owners.** Field types are owned slots. Borrows cannot be stored in fields — this avoids inter-procedural lifetime annotations on structs.
- **Field assignment transfers.** `p.field = x` must transfer: either `p.field = #x` (explicit) or `p.field = new T(...)` (auto-promoted). Plain `p.field = y` where `y` is a named borrow is a static error.
- **Field reads borrow.** `String n = p.field` makes `n` a borrow rooted at `p`.

---

## Containers (stdlib convention)

All stdlib containers follow:

- `void add(#T element)` — transfer in.
- `T get(int i)` — returns borrow, lifetime tied to receiver.
- `#T remove(int i)` — transfer out.
- `for (T x : container)` — `x` is a borrow per iteration, bounded by the loop body.

A borrowing-container type may exist separately, but the default `List<T>` etc. own their contents.

---

## Struct views (zero-copy)

A `struct` type can be constructed as a view over a byte buffer. The view borrows the buffer; field accesses are direct loads against the buffer's bytes — no allocation, no copying. This is the zero-copy path for wire-format parsing on high-throughput servers.

Full layout/endianness/packing/validation rules: see `WireFormats.md`. The interaction with the borrow model:

```
byte[] bytes = network.read();          // bytes is owner
MyStruct s = MyStruct(bytes);           // s is a borrow-view of bytes
s.version;                              // borrow into bytes, lifetime tied to bytes
s.name;                                 // borrow into the same buffer (inline String)
```

- `MyStruct(byte[])` is a compiler-synthesized view constructor.
- `s` is a borrow of `bytes`.
- `s.field` returns a borrow rooted at `bytes` per the field-path rule.
- When `bytes` drops, `s` becomes invalid — static checker rejects later use.

**No `#` is needed at construction:** the constructor returns a borrow, the assignment is a borrow per the default `=` rule, and `bytes` retains ownership.

### Alias-mutation on shared buffers

The path-based alias-mutation rule applies automatically. While `s` is a live borrow of `bytes`:
- `bytes[i] = X` mutates the borrow-source path → static error.
- `s.field = X` mutates `bytes` via a sub-path → invalidates any other live borrows of `bytes` (or its sub-paths); the checker catches it.

Single-mutator-on-shared-buffer semantics fall out without new rules.

### Owning variant (additive)

If the struct takes the buffer by transfer, it becomes both the view and the buffer's owner:

```
MyStruct s = MyStruct(#bytes);   // takes ownership; bytes is moved
// s owns the buffer; when s drops, bytes drops with it
```

Two constructors per struct, picked by whether the callsite uses `#`:
- `MyStruct(byte[])` — borrow-view; original owner unchanged.
- `MyStruct(#byte[])` — owning-view; struct drops the buffer when it drops.

v1 ships borrow-view only; the owning variant is purely additive (no codegen rework, only an additional signature).

---

## Runtime: drop chain with watermark

### Layout

Per-thread (v1: global, single-threaded) linked list of cleanup entries, parallel to the exception frame stack.

```c
struct DropEntry {
    void* obj;
    void (*drop_fn)(void*);
    struct DropEntry* prev;
    bool active;
};

static struct DropEntry* __cajeta_drop_top = NULL;
```

Each `DropEntry` is stack-allocated (alloca) in its declaring frame.

### Codegen patterns

```
// Owner declared
DropEntry e = { &obj, &drop_T, __cajeta_drop_top, true };
__cajeta_drop_top = &e;

// Move-out via `#`
e.active = false;

// Normal scope exit (per-scope, LIFO over entries)
for each entry e in this scope, in reverse declaration order:
    if (e.active) drop_fn(e.obj);
    __cajeta_drop_top = e.prev;

// try block entry
exc_frame.drop_watermark = __cajeta_drop_top;
setjmp(exc_frame.buf);

// throw (runtime helper, before longjmp)
while (__cajeta_drop_top != exc_top->drop_watermark) {
    if (__cajeta_drop_top->active) {
        __cajeta_drop_top->drop_fn(__cajeta_drop_top->obj);
    }
    __cajeta_drop_top = __cajeta_drop_top->prev;
}
longjmp(exc_top->buf, 1);
```

### Special cases

- **Drop-during-drop:** if a `drop_fn` throws, the runtime aborts. Same as C++ noexcept-violation.
- **Move-out then throw:** entries with `active = false` are skipped during unwind.
- **Catch handler's locals:** catch runs in the same function frame; owners declared inside the try block were unwound by the throw; owners declared before the try are still active.
- **Rethrow:** standard — walks up to the next outer try.
- **Return through a try:** normal scope exit pops + drops; `__cajeta_exc_pop` runs alongside.

---

## Debug-mode runtime checks (build flag: `--debug-borrows`)

A separate build path verifies the static checker by adding generation tracking.

- Every heap object gains an 8-byte `generation` field.
- Every borrow snapshots the source's generation at creation.
- Every borrow access compares snapshot to current; mismatch aborts with diagnostic.
- Owner drop bumps the generation before freeing.

Cost: +8 bytes per heap object, ~one load + compare per borrow access. **Debug builds are not ABI-compatible with release builds** — accepted trade for tool simplicity.

---

## Errors caught statically (summary)

| Error | Caught by |
|-------|-----------|
| Use-after-free | Scope-based lifetime check |
| Use-after-move | Per-variable moved-state tracking |
| Double-free | Impossible by construction |
| Alias-mutation invalidation | Path-based borrow tracking |
| Drop-order error | LIFO scope analysis |
| Borrow-of-frame-local returned | Signature conformance check |
| Anonymous-owner chained borrow | Expression-level lifetime check |
| Borrow stored in long-lived owned field | "Fields are owners" rule (field type bans borrows) |

---

## Deferred / out of scope (v1)

- Multi-threading: needs a `Send`/`Sync`-style protocol. Runtime layout already accommodates per-thread state when added.
- FFI safety: checker trusts FFI signatures.
- `unsafe` escape hatch: not in v1.
- Reflection / dynamic-dispatch borrow analysis: revisit when virtuals land.
- Cycles: forbidden by single-ownership; documented behaviour, no runtime enforcement needed.

---

## Rollout

Existing Cajeta code (268 tests, all Java-idiom — no `#`, no borrow/owner distinction) doesn't conform. Two paths:

1. **Migration (big-bang).** Rewrite all tests + stdlib to use the new model in one PR. Cleaner end state; large patch.
2. **Opt-in legacy mode.** Code without `#` runs in "trust me" mode (no borrow checking); new code opts into checked mode via a file-level pragma or compiler flag. Migrate incrementally.

Recommend **path 1**. The codebase is small enough; the inconsistency of path 2 isn't worth the maintenance overhead.

**Implementation order:**

1. **Parser:** add `#` token in value-prefix position; allow it as a type-prefix in parameter and return position.
2. **AST:** extend `Expression` with a `moveFlag` bit; extend signatures with a `transferred` bit on parameter and return types.
3. **Static analysis pass:** add a borrow-checker that runs after `resolveTypes`. Intra-function first (scope + path tracking); inter-procedural elision second.
4. **Runtime:** define `DropEntry`, add push/pop helpers, extend the exception frame with a `drop_watermark` field. Update `__cajeta_throw` to unwind drops before longjmp.
5. **Codegen:** at each owner declaration, emit DropEntry alloca + chain push. At each scope exit, emit pop+drop. At each `#` move-out, emit `active = false`. At each try-block entry, save watermark. At each return, emit drops for the function's still-active owners.
6. **Migration:** rewrite stdlib runtime helpers (string concat, substring, etc. currently leak) to integrate with drops. Rewrite test suite to use the new ownership idioms.
