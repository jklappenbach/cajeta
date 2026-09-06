# Returning stack-allocated values — design

_Branch: `stack-borrowing`. Driver: `cajeta.concurrent.Channel<T>.receive()` wants to
hand back a `stack Optional<T>` (no per-item heap alloc), which today is rejected
with `CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER`._

## Requirement

> If a method constructs a `stack` allocation and returns it, the returned value is
> stack as well.

i.e. a `stack` value returned by a method is a **by-value copy** in the caller's
frame. No `#`. No dangling pointer. Storage class **propagates through `return`**
just as it already does through assignment and parameter passing.

## Why it's rejected today

The return model has exactly two forms:

| form | meaning | ABI |
|---|---|---|
| `T f()` | borrow — pointer tied to a source's lifetime | returns a `ptr` |
| `#T f()` | ownership transfer — heap | returns a `ptr` |

Both return a **pointer**. A fresh `stack` value returned as a pointer would point
into the dying stack frame → use-after-free, so the compiler rejects it
(`Statement.cpp:1180`, comment "stack memory dies on function return"). **That
rejection is only correct under the return-by-pointer assumption.** Returning the
value **by copy** is safe — the bytes land in the caller before the frame dies.

## The one unifying rule (no special-case path)

There is now **one kind of type — `class`** (the `struct` value-type keyword has
been removed). So storage class (stack vs heap) is the **only** axis left, and it
governs how a value crosses *every* boundary:

- **stack value → copy semantics.** Assignment, parameter passing, and `return`
  are all copies. No borrow tracking, no `#`, cannot dangle (copies are
  independent).
- **heap value → reference semantics.** `=` borrows, `#` transfers, the single
  owner frees.

"Returning a value" is therefore **not a new feature** — it is the uniform
behaviour of a stack value, applied at one more boundary. The fix is to make
`return` obey the storage-class rule already used at assignment/params, and to drop
the return-by-pointer-only assumption. (This is the opposite of the rejected
approach, which bolted a third return path — a `returnsValue` flag + body scan +
special ABI — onto the borrow/`#` model.)

## What must change in all cases

1. **Borrow checker:** a `stack` return is a copy, not a dangling borrow — remove
   the `FRESH_RETURN_NEEDS_TRANSFER` rejection for stack/value returns.
2. **Return ABI:** a value (stack) class return travels **by value** (the struct
   bytes), not by pointer.
3. **Caller receive:** the returned value lands in the caller's **stack slot** (a
   struct alloca), copied in.
4. **Drop:** a stack value with heap-owning fields still drops those fields at the
   *caller's* scope-exit; a pure-value stack (`Optional<int32>`) needs no drop.

## Ways to handle the ABI mechanism

### Way 1 — by-value small-struct return, reusing the interface path  *(recommended)*

Cajeta **already** returns interface fat-pointers (24-byte bodies) **by value** via
the small-struct ABI (`Method.cpp:629`, caller "repackag[es] into a fresh body
alloca"). Generalize that exact path to value (stack) class returns:

- callee: LLVM return type is the struct itself; load the struct from its stack
  alloca and `ret` the value (the existing AllocaInst-load at `Statement.cpp:1435`
  already produces a struct value from a `stack` alloca);
- caller: store the returned struct value into the receiving local's struct alloca
  (the same repackage the interface path does).

**How the signature knows — the unification, not a flag:** the return's storage
class is resolved in the existing type-resolution pass — the same pass that already
knows `stack Foo()` is a value and `heap Foo()` is a reference. The return ABI just
asks "is the returned value a value or a reference?" — the *identical* storage-class
query already made at assignment and parameter boundaries. One rule, asked at one
more place; no `returnsValue` bolt-on.

Tradeoff: one copy (callee stack → caller stack); for small types (Optional ≈ 8
bytes) negligible, often returned in registers.

### Way 2 — caller-allocated result slot (sret / NRVO)

This realizes the requirement *literally*: "the return allocation should be stack as
well" becomes "the return value's stack home **is the caller's frame**, and the
callee constructs directly there — one allocation, no copy."

**Mechanism (the sret ABI).** A value-returning method
`Optional<T> receive()` lowers to an LLVM function with a **hidden leading pointer
parameter** and a `void` result:

```
declare void @receive(ptr sret(%Optional.int32) %ret, ...real args...)
```

- The **caller** allocates the result storage in its own frame (`%o = alloca
  %Optional.int32`) and passes its address as `%ret`.
- The **callee** writes its result through `%ret` and `ret void`. After the call,
  the caller's slot already holds the value — no load, no store, no copy.
- `sret(T)` is a real LLVM parameter attribute; it tells the backend + optimizer
  "this pointer is the return slot," enabling alias/lifetime reasoning.

**Callee codegen — two flavors:**

1. *Naïve sret:* construct `stack Optional<T>(...)` into a local alloca, then
   `memcpy` it into `%ret`, `ret void`. One copy (callee-local → `%ret`).
2. *NRVO (named return value optimization) — the real prize:* recognize that the
   `stack` construction is **destined for return** and point the constructor at
   `%ret` directly. `return stack Optional<T>(true, item)` writes its fields
   straight into the caller's slot — **zero copy**. Channel's two
   `return stack Optional<T>(...)` branches both target the same `%ret`.

**Caller codegen.** `Optional<int32> o = ch.receive();`:
- `o`'s storage is a struct alloca in the caller's frame;
- pass `&o` as `%ret`; the call returns `void`; `o` is already populated.
- For an unnamed temporary (`ch.receive().isPresent()`), the caller materializes a
  hidden temp slot, passes it, then uses it.

**Borrow checker / drop:** identical to Way 1 — `%ret` is caller-owned and outlives
the call, so nothing dangles; a value with heap-owning fields drops those at the
caller's scope-exit; pure-value `Optional<int32>` needs no drop.

**Where Way 2 wins, and the crucial overlap with Way 1.** The platform C ABI
(x86-64 SysV) **already** returns structs larger than 16 bytes via an implicit sret
pointer. So if Way 1 simply sets the LLVM return type to the struct value and lets
LLVM lower it, the backend *automatically* uses registers for small structs
(≤16 B, e.g. `Optional<int32>` ≈ 8 B) and an implicit sret for large ones — we get
both regimes for free without hand-writing the hidden param. What Way 2 adds **on
top** of that is explicit **NRVO**: eliding the callee-side construct→sret copy that
even the large-struct ABI otherwise performs. So:

- For **small** value types: Way 1 (register return) is cheapest; an explicit sret
  would add a memory round-trip. Way 2 only ties Way 1 here if NRVO elides.
- For **large** value types: both end up on sret at the ABI level; Way 2 + NRVO is
  strictly better (no construct copy), Way 1 leaves one copy to the optimizer.

**Cost of Way 2 beyond Way 1.** The ripple is confined to **value-returning (sret)
methods** — methods returning `void`, a `heap`/`#T` pointer, a primitive, or a
borrow are untouched (same ABI, same call sites). And "allocate a slot" is usually
not *extra* work: a named receiver (`Optional<int32> o = receive()`) needs stack
storage for `o` anyway, and under sret that local's slot simply *is* the `%ret`
target (this is what NRVO leans on); only an unnamed temporary needs a synthesized
slot, and Way 1 needs somewhere to land its returned value too — a wash.

The genuine asymmetry is that the hidden sret pointer is *part of the function
type*, so for value-returning methods it ripples where Way 1's return-type-only
change does not:
- **function-pointer / method-reference types** for value-returning methods must
  include the sret param (indirect calls + `obj::method` must match);
- **vtable slots:** if a value-returning method is virtual, every override must
  share the sret ABI;
- the return type becoming `void` interacts with expression contexts that expect a
  value (must thread through the temp-slot rewrite above).

Way 1's "set return type = struct, let LLVM lower" keeps the signature returning a
value, so it touches none of the function-type / vtable / method-ref machinery and
lets the backend choose registers-vs-implicit-sret — which is why it's the better
*first* step.

_(A former third option — "make small value types `struct`" — is moot: the `struct`
value-type keyword has been removed; everything is a `class`. This is why the
storage-class axis must carry value semantics for classes directly.)_

## Open question — per-method commitment

A class can be `stack` *or* `heap` per construction site, so a method must commit to
one return ABI. Rule: the method's return storage class is the storage class of its
return expression(s); they must agree. A method that returns a `stack` value in one
branch and a `heap`/borrow in another is a type error (or must use `#` and go
heap). For uniform methods (`Optional`/`Channel`: every return is `stack`), it's
unambiguous — which is the case that matters now.

## What shipped

**Way 2 — explicit sret + NRVO.** A value-returning method lowers to a `void`
LLVM function taking a hidden `ptr sret(struct)` at arg 0 (before `this`); the
returned `stack X(...)` constructs **directly into the caller-owned slot** (NRVO,
zero copy), with a memcpy fallback for non-construction stack returns. The
`FRESH_RETURN_NEEDS_TRANSFER` rejection is bypassed for stack returns.
`CajetaClass::invokeMethod` self-allocates the result slot when none is supplied
and returns a pointer to it — so the result behaves like any class-instance
pointer, and the local/assignment/member-access sites need no change. The
determination (which method returns by value) is the body scan over `return stack
X(...)` described above, because storage class lives on the construction expression
in the grammar (`STACK (creator | aggregateInitializer)`), not on the type.

Way 2 was chosen over Way 1 to guarantee zero-copy on **large** value returns
(NRVO elides the construct→sret copy that even Way 1's implicit-sret would
otherwise perform), accepting the function-type ripple. Way 1 — generalizing the
interface by-value path — remains documented above as the path not taken; it would
have been cheaper in surface area and faster for the small-return regime (register
return) but ties Way 2 only when NRVO fires.

**Status:** shipped on `stack-borrowing` —
- `f9cebc3` direct-dispatched value returns (sret + NRVO core).
- `61a4e1f` virtual dispatch for value-returning methods (M5(a) — sret-shaped
  vtable slots).
- `07d2240` caller-scope stack-drop for value-returned locals (M5(c) — owned
  fields fire at scope exit without freeing the alloca).
- `b435772` function-pointer / method-reference types carrying sret (M5(b)
  codegen): lambda body-scan picks sret form when the body is a
  `stack X(...)` construction; method references to value-returning methods
  produce sret-shaped function-types; the indirect-call sites allocate the
  result slot and thread it as the closure's hidden arg 0.
- `c075b7e` function-type syntax (`(P) -> R` sret form vs `(P) -> #R`
  ownership form). The grammar's REFERENCE flag on `typeTypeOrVoid` is now
  threaded through `CajetaType.cpp` and embedded in the canonical name;
  non-sret-eligible returns (primitive, void, interface, array, view) are
  normalized to ownership so `(T) -> boolean` doesn't spuriously split.
  Source migration of existing `(P) -> R` class-return sites to `(P) -> #R`
  came with this commit.
- `(this)` borrow→sret adapter for method references: `b::peekBorrowMethod`
  bound into `() -> R sret` (no `#`) synthesizes a thunk that calls the
  borrow method and memcpys the returned `R*` into the caller-owned sret
  slot. Matrix-rejected combinations (`#R` method → sret slot;
  stack-value method → `#R` slot) throw `CAJETA_ERROR_TYPE_MISMATCH` at
  resolveTypes via the expected-type hint LocalVariableDeclaration now
  forwards to MethodReferenceExpression.

This keeps **one way**: storage class (stack = copy / heap = reference) is the
single dimension, now governing `return` identically to assignment and parameters.

## The relay gap, closed (2026-09-06)

The body scan cannot see a **relay**: a method whose body returns an Optional it
obtained from a call has no `stack` in it.

```cajeta
Optional<int32> take() { return this.ch.receive(); }   // receive() is sret
```

Under the scan alone `take` was typed as a pointer return while its body `ret`
the struct `receive` produced through sret. LLVM's verifier rejects that IR
("Function return type does not match operand type of return inst"); where
nothing verified, the caller read the struct bits as a pointer and the Optional
arrived empty — measured in cabra's LineReader (2026-08-31) and in nine
`KernelManifest` accessors (2026-09-06; the nine errors printed under other
`Optional<…>` names because a merged module shares one LLVM struct between
isomorphic instantiations — `Optional<int32>` and `Optional<Severity>` are both
`{ ptr, i1, i32 }`).

The determination is now **decided by the return type for the value-shape
class**: a method, an interface declaration or an inferred lambda that returns
`cajeta.lang.Optional<T>` (any instantiation) is sret whatever its body does
(`Method::isValueShapeReturnType`, the predicate the interface path already used
under #63). The body scan still turns sret on for any other class returned by
`return stack X(...)`, and an explicit `(P) -> R` / `(P) -> #R` on a lambda's
LHS stays authoritative. The relay's `return <call>` lands in the sret path's
non-construction fallback, which copies the callee's value into the caller's
slot.

Two shapes have nothing to copy and are rejected at the return, not at the first
caller's segfault:

- `return null;` in a by-value method — `CAJETA_ERROR_NULL_RETURN_BY_VALUE`;
  write `return stack Optional<T>(false);`.
- `return heap X(...);` in a by-value method — `CAJETA_ERROR_HEAP_RETURN_BY_VALUE`;
  write `return stack X(...)`, or declare the return `#T` to hand the caller an
  owned heap object (that form is untouched by the rule).

`test/parser/OptionalRelayReturnTests.cpp` pins all of this: static-helper and
generic-instance relays keep flag and value, a two-hop relay through `orElse`,
the relay's `define void … sret(` shape in IR, the two rejections, the owned
`#Optional<T>` form still compiling, and an inferred lambda relaying an sret call.
