# Cajeta Memory Model — Specification v1

## Goals

- **Single-owner heap.** Every heap-allocated value has exactly one owning reference at any time.
- **Implicit borrow, explicit transfer.** `=` borrows by default; the `#` operator transfers ownership.
- **Static safety, no user-visible annotations.** All borrow/lifetime errors are compile-time. Scope-based inference; no lifetime syntax for users to write.
- **Zero runtime cost in release.** Heap blocks are plain memory; drops happen at owner scope-end. A debug build flag adds runtime verification for testing the static checker.

> **AMENDED (2026-07-03): a THIRD ownership state — `shared`** (governed by
> [`slice-spec.md`](slice-spec.md), approved 2026-07-02). Cajeta's model is an **implicit
> smart-pointer family** — the kind is always inferred, never annotated:
>
> | State | Discipline | Analog |
> |---|---|---|
> | **owned** | single responsible dropper; `#` transfers the stake | `Box`/`unique_ptr` |
> | **borrow** | non-owning; must not outlive its source (static) | `&T` |
> | **shared** | co-owned **immutable leaf buffers**; runtime count; freed at the last stake | `Rc`/`shared_ptr` |
>
> `shared` exists for exactly one thing v1: a **slice** (`String.substring`, later `Slice<T>`)
> that outlives the value it was sliced from — the case borrow must reject and copying is too
> expensive to force. Promotion is one-way (`owned → shared`); moves are **rc-neutral**; the
> count lives in a side table keyed by buffer base with a stolen count-word sign bit (no layout
> change); buffers never sliced-and-stored pay one predicted bit-test at drop and nothing else.
> Scoped to immutable leaf buffers ⇒ the shared graph is acyclic ⇒ no cycles, no `weak`, no
> leaks. An **escaping borrow of an eligible source resolves** (copy small / share large / copy
> arena-backed) instead of erroring; identity/mutable objects keep the error-and-`#` discipline
> (slice-spec §4, §9 — the transparent-only boundary). `Object.clone()` = shallow copy +
> stake-share per String field (slice-spec §6.4; LIVE). The "single-owner heap" goal above is
> thereby refined: single owner **or counted co-owners for immutable slice backings**.

## Non-goals (v1)

- Multi-threading safety (deferred — needs Send/Sync-style protocol).
- FFI safety beyond signature-trust.
- `unsafe` escape hatch.
- Reflection / dynamic-dispatch borrow analysis (deferred until virtuals land).

---

## Operator: `#`

Single token, and — with one opt-in exception — it appears only where a transfer
actually happens: at a **use site**, never in a signature.

| Position | Meaning |
|----------|---------|
| `#expr` (value position) | Transfer the title *from* expr. After this, expr is moved; a later read is a static error. |
| `#T` (parameter type) | **Opt-in must-own.** The method refuses a lend: the caller has to surrender. Rarely needed — see below. |
| `#T` (return type) | The method always transfers. Also rarely needed; a plain return already carries whatever title it holds. |

**Transfer is the caller's decision, not the signature's.** A plain class-typed
parameter accepts *both* a lend and a transfer — which one happened is decided at
each call site by whether the argument was spelled `#x`:

```cajeta
void keep(Cell c) { this.held = #c; }   // no ownership spelling in the signature

sink.keep(cell);        // lends — `cell` still owns it, and drops it at scope exit
sink.keep(#cell);       // surrenders — the title moves; `cell` is moved-from
```

This is deliberate. An earlier design put the ownership mode in the signature and
required the author to predict, at declaration time, how every future caller would
use the method. That prediction is not available — most acutely for containers,
where the same `put` is legitimately used both ways — and the honest endpoint was
to mark *everything* "either", which is what the language now does by default.
Rationale and the rejected alternatives are recorded in the title-tracking spec §4.6.

`#T` on a parameter survives only as an **opt-in must-own** edge, for a method
that cannot function with a borrow (it stores the value somewhere that outlives
the call, and has no way to cope with the caller keeping the title). A plain
argument at such an edge is `CAJETA_ERROR_TRANSFER_REQUIRED`.

---

## Borrow / transfer rules

| Operation | No `#` | With `#` |
|-----------|--------|----------|
| `a = b` | a borrows b | a takes the title; b is moved |
| `this.f = x` | the field borrows x — the title stays with x and x still drops it | the field takes the title |
| `f(x)` | f borrows x for the call | f takes the title; x is moved |
| `return x` | hands back whatever title x held (a lent value stays lent; an owned one transfers) | hands back the title |

Note the second row: **a plain field store lends.** It does not quietly take
ownership. If a method stores a borrowed value into a field that outlives the
call, the field is left pointing at something the caller will free — which is what
the dangling-lend check below catches.

**Auto-promotion for fresh heap allocations.** An anonymous `heap T(...)` expression in transfer position promotes implicitly. `p.field = heap T()` and `return heap T()` (in a `#T` function) work without explicit `#`. The temporary is an unnamed owner with no prior identity, so promotion has no use-after-move risk.

---

## Static analysis rules

### Lifetime inference (intra-function)

The compiler tracks each owner's declaration site and scope-end. For each borrow, it tracks the source's lifetime. A borrow that may outlive its source is a compile error at the use site.

### Path-based borrow tracking

Borrows track their *path* from the named root, not just the variable. `String n = person.address.city` records `n`'s root as `person` with path `address.city`. Reassigning any link along the path (`person.address = #x`) drops everything derived from that link; subsequent use of `n` is a static error.

### Function signatures: the transfer ABI

Because the caller decides, the callee has to be *told* what it got. Every
class-typed parameter and return therefore carries a hidden per-call flag.

- **Arguments.** A method with at least one pass-by-pointer class parameter takes
  a hidden trailing word; bit *i* is set iff user-argument *i* was surrendered.
  `@Kernel`, `@Device`, `@Native` methods and `static main` keep the plain C ABI —
  the compiler does not own both sides of those boundaries.
- **Returns.** A method returning a class pointer stores a paired flag beside the
  return value. (This one is a thread-local: nothing runs between the callee's
  `ret` and the caller reading it, so there is no window to corrupt.)

**Formals and call results are *runtime* owners.** A class-typed parameter is not
statically a borrow and not statically an owner — it is whichever the caller made
it, so its drop entry is *armed from its flag bit* on entry. The same is true of a
local initialized from a call: it arms from the return flag.

The consequences follow from that one rule:

- A surrendered argument the callee never consumes **drops in the callee**, on
  whatever exit it takes — including a `throw`, where the runtime unwinder walks
  the drop chain.
- A lent argument leaves the entry disarmed, so the callee's scope exit does not
  touch it. The caller still owns it.
- `#v` inside the callee (a store, a forward, a return) **consumes** the formal:
  it reads the flag, deactivates the entry, and passes that same flag on. A
  forwarding chain therefore threads the *caller's* decision all the way down —
  a value lent into `outer` and forwarded with `#` to `inner` arrives at `inner`
  still lent, and nobody frees it.

Ownership is never *inferred* from the body. The compiler could often guess (a
lend at a local's last use is usually a transfer the author forgot to spell), but
guessing is wrong in exactly the cases that matter — a value handed to a spawned
task outlives the frame that appears to be done with it. So the compiler advises
instead of acting: see the last-use advisory below.

### Dangling lends

A plain store or a plain argument **lends**. If the thing that received the lend
then escapes the method, it escapes holding a pointer to a local that is about to
drop:

```cajeta
Holder build() {
    Holder h = heap Holder();
    Cell s = heap Cell(5);
    h.c = s;          // lend: the title stays with `s`
    return #h;        // ERROR — CAJETA_ERROR_DANGLING_LEND
}                     // `s` drops here; the caller's Holder points at freed memory
```

The fix is to say what was meant: `h.c = #s` gives the holder the title.

The check is intra-procedural and deliberately conservative. It fires only when
the receiving callee actually **retains** the argument (stores it into a field);
a method that merely reads its argument — `sb.append(s)`, `list.contains(x)` —
cannot strand anything and does not poison its receiver.

### Last-use advisory (warning)

Lending a local at its **final use** is suspicious: nothing in the scope reads it
again, so the lend usually should have been a transfer. The compiler says so and
moves on — `CAJETA_WARN_LAST_USE_TRANSFER`, with a `#` fixit. It is a warning, not
an error: the build stays green, because (per above) the compiler cannot know
intent. A later read of the local suppresses it, as does a use inside a loop
(where the "last" textual use runs again next iteration), as does spelling `#x`.

### Mode-only overloads are rejected

Transfer mode is not part of a signature — dispatch erases `#` — so two
declarations that differ only in mode collide:

```cajeta
void f(Cell c) { }
void f(#Cell c) { }   // ERROR — CAJETA_ERROR_TRANSFER_MODE_OVERLOAD
```

Keep one. A plain formal already accepts both a lend and a transfer.

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

- **Fields may be owners or borrows.** A field's ownership status is resolved at drop time, not at declaration. The **global live-set** is the registry: every `heap` allocation is recorded in it. At parent drop, the synthesized auto-drop wrapper calls each owned-shape field's drop dispatcher directly; the dispatcher does an atomic *claim* (remove-if-present) on the field's address. The first caller to claim an address frees it (and runs `~Class()`); a later caller for the same address — the owning local's own chain pop, or another field aliasing it — finds it already gone and no-ops. See `docs/specification/lang/FieldOwnership.md`.
- **Field assignment.** `p.field = #x` and `p.field = heap T(...)` make the field an owner — the fresh `heap` allocation is the live-set registration. `p.field = y` where `y` is a borrow stores the borrow; the field aliases `y`'s source, and the live-set claim at drop ensures whichever path reaches the shared address first frees it while the rest no-op.
- **Field reads borrow.** `String n = p.field` makes `n` a borrow rooted at `p`.
- **Use-after-free of an aliased field whose source has already dropped is the programmer's responsibility at v1.** A lifetime tracker (Phase 6+) will catch this statically.

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

- **Virtual dispatch on drop.** ✅ Done. The vtable header carries a dedicated `drop_fn` slot (index 3, byte offset 16; see `StructureMetadata::createVirtualTableType`). Heap class locals register the runtime helper `__cajeta_class_virtual_drop`, which loads the instance's vtable pointer and dispatches through `vtable.drop_fn` — so `Base b = heap Derived()` fires `~Derived()`, not `~Base()`. Pinned by `test/parser/VirtualDropDispatchTests.cpp`. Stack allocations stay on static dispatch (alloca size fixes the dynamic type); Task<T>-style custom layouts opt out via `CajetaClass::hasVtablePointerAtSlotZero()`.
- **Automatic field drops via live-set claim.** ✅ Done. `CajetaClass::getOrCreateDropFunction` → `emitDropBodyInline` synthesizes an auto-drop body that, in reverse declaration order, calls each owned-shape field's drop dispatcher directly: `__cajeta_free_array` (arrays), `__cajeta_iface_drop` (interface fields), `__cajeta_class_virtual_drop` (class-ref fields). Each dispatcher does an atomic claim against the global live-set (`__cajeta_live_set_claim`): the first caller to claim a field's address frees it and runs its destructor; any later caller for the same address (the owning local's chain pop, or another field aliasing it) no-ops. That claim is what keeps aliased stdlib fields — `ArrayStream.data` aliasing `ArrayList.data`, `Optional.value`, `Pair.first/second` — from double-freeing. Doctrine and walk-throughs in `docs/specification/lang/FieldOwnership.md`. Pinned by `test/parser/AutoFieldDropTests.cpp`.
- **Implicit destructor chaining — shipped 2026-05-21.** ✅ Done. C++ semantics. The compiler-emitted heap-drop wrapper runs: (1) this class's `~Class()` body and its own field auto-drops (in reverse declaration order), (2) every transitive ancestor's same body+own-field contribution in reverse-DFS deduped order, (3) `__cajeta_free(instance)`. Each ancestor runs exactly once — diamond-shared ancestors via the vbase ABI's single canonical sub-object are visited only on whichever branch sees them first. Chaining is automatic and non-suppressible. `super<Base>.~Base()` may be written explicitly for documentation, but it does not change codegen. The stack-drop wrapper has the same shape minus the free. Implementation: `CajetaClass::emitDropBodyInline` + `CajetaClass::collectDestructorChain`, both called from `getOrCreateDropFunction` and `getOrCreateStackDropFunction`. Pinned by `test/parser/DestructorChainTests.cpp` (9 tests, including multi-inheritance reverse-decl order and the diamond runs-once case).
- **Block-scoped firing.** ✅ Done. Drop entries fire at the closing `}` of the declaring lexical block, not method exit. RAII patterns like back-to-back `LockGuard`s in inline blocks now work. Pinned by `test/parser/BlockScopedDropTests.cpp`.

### No try-with-resources

Cajeta does **not** have Java's `try (R r = …) { … }` syntax. It was briefly in the grammar and was removed 2026-05-20 as strictly redundant: destructors already guarantee deterministic cleanup at the closing `}` of the declaring block, including LIFO order across multiple locals and on exception unwind. The Java construct exists because Java has GC and no destructors — `AutoCloseable.close()` needs an external guarantee-mechanism that the drop chain already provides here.

The replacement pattern is "just declare the resource":

```cajeta
{
    FileReader r = File.openRead(in);
    FileWriter w = File.openWrite(out, OpenMode.WRITE);
    int32 n = r.read(buf, 4096);
    while (n > 0) {
        w.write(buf, n);
        n = r.read(buf, 4096);
    }
    // w.~FileWriter() fires here (flush + close), then
    // r.~FileReader() (close). LIFO order, guaranteed on every
    // exit path (return, throw, fall-through, break).
}
```

`r.close()` is still callable for early release — destructors are idempotent (the standard Phase-A `FileReader` / `FileWriter` / `File` contract: `this.fd = -1` after the first close, subsequent calls no-op). Catch blocks still work without modification:

```cajeta
try {
    FileReader r = File.openRead(p);
    process(r);
    // r drops here on the normal path.
} catch (IoException e) {
    // r already dropped — the throw walked the chain back
    // to the try-frame's watermark, firing every owned local
    // along the way.
    log(e);
}
```

---

## Containers (stdlib convention)

Containers carry **no ownership spelling in their signatures** — they are the
clearest case for caller discretion, since the same container is legitimately used
both to own its elements and to index values owned elsewhere:

- `void add(T element)` — the *call site* decides: `add(x)` lends, `add(#x)` gives
  the container the title. The entry records which, per element.
- `T get(int32 i)` — hands back whatever the entry holds: a borrow if the entry is
  borrowed, the title if it is owned.
- `T remove(int32 i)` — membership ends; the return carries the entry's title if it
  had one.
- `for (T x : container)` — `x` is a borrow per iteration, bounded by the loop body.

There is no separate "borrowing container" type. One `HashMap<K, V>` holds owned and
borrowed entries side by side, and drops exactly the ones it owns.

---

## Structs

A `struct` is a stack-allocated value aggregate (full spec: `docs/specification/lang/Views.md`). Its interaction with the memory model:

- **Lifetime is the enclosing scope.** A struct local is allocated via `alloca` and drops at scope exit — same as any other stack-resident owner.
- **Fields participate in drop chain.** A struct field that holds an owned class reference is itself an owner; when the struct drops, owned class fields drop in reverse declaration order before the struct's bytes are reclaimed.
- **Borrowed class refs are tracked.** A struct field holding a borrowed class reference contributes to the path-borrow tracker; the borrow is rooted at whatever the field was assigned from.
- **Embedded structs in class fields.** A struct used as a class field lives inline in the class's heap layout — no extra allocation. When the class drops, every embedded struct field drops with it (recursively).
- **Field-path moves apply.** `#s.field` invalidates `s.field` for subsequent reads (existing `markMovedPath` machinery); siblings of the moved field remain readable.
- **Pass-by-pointer.** Structs cross call boundaries via the existing aggregate pass-by-pointer rule; defensive copies happen at callee entry only when the callee mutates the parameter.

The borrow checker treats structs uniformly with primitive locals — the only difference is that a struct's bytes can transitively own other resources, which the drop chain unwinds in declaration order.

---

## Views

A `view` is a typed overlay onto a borrowed byte buffer — an `int8[]` (full spec: `Views.md`). Two construction forms:

- **Borrow form** — `RpcHeader h = RpcHeader(buf);` borrows `buf`. `h` is a path-borrow rooted at `buf`; the borrow checker enforces `h` cannot outlive `buf`, the buffer cannot be mutated through any other alias while `h` is live, and `h` cannot be sent to another fiber.
- **Owning form** — `RpcHeader h = RpcHeader(#buf);` transfers `buf`'s ownership into `h`. The view is now an owner; standard transfer rules apply (`#h`, drop-on-scope-exit drops the contained buffer).

Per-field access (`h.magic`, `h.payload[i]`) is a path-borrow rooted at the buffer — same machinery as struct-field access. Mutating one field while a borrow into another field of the same view is live is allowed (different paths); mutating the buffer directly while any view borrow is live is rejected (alias-mutation).

Construction-time bounds and length-prefix validation are documented in `Views.md` § Security; they are the load-bearing guarantee that makes per-access reads bounds-check-free.

---

## Runtime: drop chain with watermark

### Layout

Linked list of cleanup entries. The chain head is **TLS + per-fiber**: `__cajeta_main_drop_top` (declared `__thread` in `cajeta_runtime.c`) is the main thread's head, and `__cajeta_drop_top_ptr()` returns the running fiber's `cajeta_fiber.drop_top` slot when one is active, falling back to the TLS main slot otherwise. Each `DropEntry` is alloca'd in the declaring function's frame, so a fiber's entries sit on the fiber's own stack and the per-fiber head pointer keeps the chains independent — safe under multi-carrier execution as long as each fiber's chain is touched by at most one carrier at a time.

```c
struct cajeta_drop_entry {
    void* obj;
    void (*drop_fn)(void*);
    struct cajeta_drop_entry* prev;
    char active;
};

// Main-thread head (per-fiber heads live in `cajeta_fiber.drop_top`):
static __thread struct cajeta_drop_entry* __cajeta_main_drop_top = NULL;
// __cajeta_drop_top_ptr() returns the running fiber's slot, or this one.
```

Each `cajeta_drop_entry` is stack-allocated (alloca) in its declaring frame.

### Codegen patterns

Conceptual shape (the real push/pop go through runtime helpers
`__cajeta_drop_push` and `__cajeta_drop_pop_run`, which read the active
head via `__cajeta_drop_top_ptr()`):

```
// Owner declared — entry alloca'd in the frame, pushed onto the active head
cajeta_drop_entry e = { &obj, &drop_T, *top, /*active=*/1 };
*top = &e;                                  // __cajeta_drop_push

// Move-out via `#`
e.active = 0;

// Normal scope exit (per-scope, LIFO over entries) — __cajeta_drop_pop_run
for each entry e in this scope, in reverse declaration order:
    if (e.active && e.drop_fn) { drop_count++; e.drop_fn(e.obj); }
    *top = e.prev;

// try block entry
exc_frame.drop_watermark = *top;
setjmp(exc_frame.buf);

// throw (runtime helper, before longjmp)
while (*top != exc_top->drop_watermark) {
    if ((*top)->active) (*top)->drop_fn((*top)->obj);
    *top = (*top)->prev;
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

## Runtime checks (debug aids)

The runtime ships two opt-in debug aids that harden the drop machinery and
help catch use-after-free during testing. Both are off by default and toggled
at runtime, so they do **not** change object layout or ABI:

- **Poison-on-free** (`__cajeta_set_poison_free(1)`). On every free, the
  reclaimed block is overwritten with `0xDB` up to its allocator-tracked
  chunk size before `free()`. A subsequent read through a dangling field
  sees the poison pattern instead of stale-but-plausible data. Implemented
  in `__cajeta_poison_buffer`.
- **Drop-chain validation** (`__cajeta_set_drop_chain_validate(1)`).
  `__cajeta_drop_pop_run` asserts the popped entry is the chain head and has
  a sane `active` flag, catching out-of-order pops, double-pops, and bit-rot
  (`CAJETA_ERROR_DROP_CHAIN_*` corruption traps).

Double-free across aliased fields is prevented unconditionally (not just in
debug) by the global live-set claim described under § Runtime.

> **Planned (not yet built):** a generation-counter borrow checker — every
> heap object carries a generation word, borrows snapshot it at creation, and
> each borrow access compares snapshot to current. This is the `R1` proposal
> in `docs/specification/lang/BorrowSoundness.md`, not shipped today.

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
| Double-free of aliased field | Runtime live-set claim (see `FieldOwnership.md`) |
| Use-after-free of aliased field whose source dropped first | Programmer responsibility at v1 (Phase 6+ lifetime tracker) |
| Escaping holder retaining a lend of a dying local | Single-hop dangling-lend check (`CAJETA_ERROR_DANGLING_LEND`) |
| Plain argument at a must-own (`#T`) edge | `CAJETA_ERROR_TRANSFER_REQUIRED` |
| Two declarations differing only in transfer mode | `CAJETA_ERROR_TRANSFER_MODE_OVERLOAD` |
| Lend at a local's last use (**warning**, not an error) | `CAJETA_WARN_LAST_USE_TRANSFER` — suggests `#`; never fails the build |

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

- **String stdlib helpers leak fix.** ✅ Done. `LocalVariableDeclaration` recognizes the owned-allocating shapes — binary `+` lowered to `__cajeta_str_concat`, and the routed method intrinsics `substring` / `toUpperCase` / `toLowerCase` / `trim` / `replace` on a String receiver — and registers `__cajeta_free` as the local's drop fn. String literal aliases and `p.name` field-reads remain borrow-shaped (no drop). Pinned by `test/parser/OwnedStringDropTests.cpp`. The pragmatic detection at the assignment site replaces the proposed type-system `OwnedString` flag for v1; the flag can land later if a non-LocalVariable owner site appears.
- **Alias-mutation through writes.** ✅ Done. `Scope` carries a `liveBorrows` map (path → borrower set) populated by `LocalVariableDeclaration` for pointer-shaped path-read initializers; `BinaryOpExpression`'s assignment branch consults `findInvalidatingBorrow` and throws `CAJETA_ERROR_USE_AFTER_MOVE` when the write path overlaps any live borrow. Pinned by `test/parser/AliasMutationBorrowTests.cpp`.
- **Multi-parameter borrow-return with annotation.** Today multi-input free functions can't return a borrow at all. Rust-style explicit lifetime annotations would lift this restriction; not part of v1.
- **FFI / `unsafe` / multi-threading.** All explicitly deferred.
- **Static class fields landed.** ✅ Done. `public static int32 total = 0;` emits an LLVM global named `<class.canonical>.<fieldName>` in the declaring class's home module via `CajetaClass::getOrCreateStaticFieldGlobal`. Cross-module references go through `ensureGlobalInModule`. `DotExpression` short-circuits class-name LHS lookups in `canonicalMap` and returns the global as an l-value for reads/writes; `loadIfLValue` treats `GlobalVariable` like a GEP slot. Static-property literal initializers (`= 100`, `= -7`, float literals) are constant-folded into the global's `setInitializer`; complex expressions fall back to zero. Statics are skipped from instance struct layout. Pinned by `test/parser/StaticFieldTests.cpp` (9 tests) plus `test/parser/LambdaStaticCaptureTests.cpp` (2 tests) for lambda-body access through globals (no captures-struct routing needed).
