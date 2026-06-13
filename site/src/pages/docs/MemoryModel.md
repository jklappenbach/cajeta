---
title: 'Cajeta Memory Model — Specification v1'
layout: '~/layouts/MarkdownLayout.astro'
category: 'Language'
description: 'Single token. Appears in three positions:'
---

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

**Auto-promotion for fresh `new`.** An anonymous `heap T(...)` expression in transfer position promotes implicitly. `p.field = heap T()` and `return heap T()` (in a `#T` function) work without explicit `#`. The temporary is an unnamed owner with no prior identity, so promotion has no use-after-move risk.

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

- **Fields are always owners.** Field types are owned slots. Borrows cannot be stored in fields — this avoids inter-procedural lifetime annotations on field-holders.
- **Field assignment transfers.** `p.field = x` must transfer: either `p.field = #x` (explicit) or `p.field = heap T(...)` (auto-promoted). Plain `p.field = y` where `y` is a named borrow is a static error.
- **Field reads borrow.** `String n = p.field` makes `n` a borrow rooted at `p`.

---

## Destructors

A class can declare a destructor with `~ClassName()`. The compiler runs it just before the instance's heap memory is reclaimed — the canonical place to release the resources the instance owns:

```
public class Lock {
    private pointer handle;

    public Lock() {
        this.handle = Cajeta.lockNew();
    }

    public ~Lock() {
        Cajeta.lockDestroy(this.handle);
    }
}
```

Rules:

- **Identifier must match the class.** `~Foo()` inside `class Bar` is a compile error. Same convention as the constructor's identifier.
- **No parameters, no return type.** The body has no inputs to take and nothing to hand back. `~Lock(int32 x)` is a parse error.
- **Not user-callable.** Calling `obj.destructor()` or similar from user code is rejected — destructors are invoked exclusively by the drop chain at scope exit.
- **Inside the body, `this` is live.** Field access, intrinsic calls, even calls to other instance methods on `this` all work. The instance hasn't been freed yet.
- **Runs once.** Each instance's destructor fires exactly once, at the point the drop chain reaches its entry. If the instance was transferred via `#` to another owner, the original owner's drop entry is deactivated; only the new owner's drop fires the destructor.

The runtime mechanism — "drop chain" — is the same machinery the borrow checker uses to reclaim arrays, closures, and other owned heap blocks. A destructor is the *user-extensible* hook into it: the compiler emits a per-class wrapper `__cajeta_<ClassName>_drop` that calls your destructor and then frees the instance's memory. From a developer's perspective, the contract is simply "write `~ClassName()` if you have a resource to release; the language guarantees it runs at the right time."

Limitations (v1 / known gaps):

- **No virtual dispatch.** Dropping a `Bar` local that holds a `Foo extends Bar` fires `~Bar()`, not `~Foo()`. Adding `drop` to the vtable is the proper fix; deferred.
- **No automatic field drops.** If a class owns a heap field (an array, a class instance, another `Lock`), the user's destructor must release it explicitly. Rust auto-generates these; we don't yet.
- **No `super.~Class()` chaining.** Derived destructors don't implicitly chain to the base class's. With single-class hierarchies this hasn't bitten yet; needs care when virtual dispatch lands.
- **Method-scoped, not block-scoped firing.** Drop entries fire at method exit, not at the closing `}` of an inner block. RAII patterns that need release-on-block-exit (a `LockGuard` declared in an inner scope releasing before the rest of the method runs) currently require splitting the critical section into its own method.

---

## Containers (stdlib convention)

All stdlib containers follow:

- `void add(#T element)` — transfer in.
- `T get(int i)` — returns borrow, lifetime tied to receiver.
- `#T remove(int i)` — transfer out.
- `for (T x : container)` — `x` is a borrow per iteration, bounded by the loop body.

A borrowing-container type may exist separately, but the default `List<T>` etc. own their contents.

---

## Structs

A `struct` is a stack-allocated value aggregate (full spec: `Structs.md`). Its interaction with the memory model:

- **Lifetime is the enclosing scope.** A struct local is allocated via `alloca` and drops at scope exit — same as any other stack-resident owner.
- **Fields participate in drop chain.** A struct field that holds an owned class reference is itself an owner; when the struct drops, owned class fields drop in reverse declaration order before the struct's bytes are reclaimed.
- **Borrowed class refs are tracked.** A struct field holding a borrowed class reference contributes to the path-borrow tracker; the borrow is rooted at whatever the field was assigned from.
- **Embedded structs in class fields.** A struct used as a class field lives inline in the class's heap layout — no extra allocation. When the class drops, every embedded struct field drops with it (recursively).
- **Field-path moves apply.** `#s.field` invalidates `s.field` for subsequent reads (existing `markMovedPath` machinery); siblings of the moved field remain readable.
- **Pass-by-pointer.** Structs cross call boundaries via the existing aggregate pass-by-pointer rule; defensive copies happen at callee entry only when the callee mutates the parameter.

The borrow checker treats structs uniformly with primitive locals — the only difference is that a struct's bytes can transitively own other resources, which the drop chain unwinds in declaration order.

---

## Views

A `view` is a typed overlay onto a borrowed byte buffer (full spec: `Views.md`). Two construction forms:

- **Borrow form** — `RpcHeader h = RpcHeader(buf);` borrows `buf`. `h` is a path-borrow rooted at `buf`; the borrow checker enforces `h` cannot outlive `buf`, the buffer cannot be mutated through any other alias while `h` is live, and `h` cannot be sent to another fiber.
- **Owning form** — `RpcHeader h = RpcHeader(#buf);` transfers `buf`'s ownership into `h`. The view is now an owner; standard transfer rules apply (`#h`, drop-on-scope-exit drops the contained buffer).

Per-field access (`h.magic`, `h.payload[i]`) is a path-borrow rooted at the buffer — same machinery as struct-field access. Mutating one field while a borrow into another field of the same view is live is allowed (different paths); mutating the buffer directly while any view borrow is live is rejected (alias-mutation).

Construction-time bounds and length-prefix validation are documented in `Views.md` § Security; they are the load-bearing guarantee that makes per-access reads bounds-check-free.

---

## Runtime: drop chain with watermark

### Layout

Linked list of cleanup entries (currently single global head — promotion to TLS is a known gap under the fiber model). Each `DropEntry` is alloca'd in the declaring function's frame, so when a fiber suspends mid-call its drop entries sit dormant on the fiber's own stack — but the global chain-head pointer is shared across fibers, which means today's drop chain assumes one fiber at a time touches it (true for the cooperative single-carrier model, but per-fiber-TLS-head is the correct long-term fix). See `ThreadModel.md` § Runtime requirements for the fiber executor.

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

---

## Known gaps (post-v1 rollout)

The rollout left a few items deliberately out of v1 scope; they're called out here so future work knows where to pick up.

- **String stdlib helpers still leak.** Functions like `__cajeta_str_concat`, `__cajeta_str_substring`, `__cajeta_str_toUpperCase` all return malloc'd memory that's never freed. Wiring them through the drop chain needs the type system to distinguish "this `String` is heap-owned" from "this `String` is a borrow/literal" — the current `String` type collapses both. A first step is a dedicated `OwnedString` flag on the type instance plus codegen that registers a drop entry only for the owned variant.
- **Alias-mutation through writes.** Path-based borrow tracking catches use-after-move; it does not yet catch "borrow into `person.name` invalidated by a later `person.name = #other` write." That needs a live-borrow tracking pass.
- **Multi-parameter borrow-return with annotation.** Today multi-input free functions can't return a borrow at all. Rust-style explicit lifetime annotations would lift this restriction; not part of v1.
- **FFI / `unsafe` / multi-threading.** All explicitly deferred.
- **Drop chain head is global, not per-fiber.** The stackful fiber executor (R3-B) added cooperative concurrency, but the drop-chain head is still a single static. Correct under today's single-carrier model (only one fiber executes at a time), but multi-carrier parallelism — or any change that lets two carriers run fibers simultaneously — needs the chain head promoted to TLS / fiber-local. Drop entries themselves are already alloca'd in the fiber's own stack, so the only piece moving is the head pointer.
