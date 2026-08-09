# mode-carrying-claim — `#=` must carry a slot's mode, not demand a title

## 1. Definition

Opened 2026-08-09, from the collection ownership reversal. `T x #= slot`
lowers to a **take**: the runtime hands back the title and nulls the
slot's bit, returning NULL when the slot holds no title. Codegen turns
that NULL into `CAJETA_PANIC_TITLE_MISS` instead of yielding the borrow.

Before the reversal this was unreachable — collection slots were always
owned. Now that collections borrow by default, a slot may legitimately
hold either, and the panic is on the common path.

### 1.1 Measured

Built against `cbe40fd1` + the ownership branch:

| shape | owned element | borrowed element |
|---|---|---|
| `Heap.pop()` | runs clean | **panics** `value=0x3` |
| `HashMap.remove(k)` | runs clean | **panics** `value=0x3` |

Both panic inside the claim, before their `return` is reached. This is
one bug in a shared protocol, not two container bugs — every
remove-shaped method claims into a local then returns it.

### 1.2 The panic is indefensible in these methods

The declared contracts:

    public T pop()            // Heap        — plain
    public V remove(K key)    // HashMap     — plain
    public T popHead()        // LinkedList  — plain
    public #T operator#[] (int32 i)   // ArrayList — PROMISES title

None of the first three promised ownership, so refusing to produce a
borrow contradicts their own signatures. `operator#[]` **does** promise
title, so a borrowed slot genuinely violates *its* contract — the panic
is correct there and must survive.

### 1.3 It contradicts the definition of `#=`

Three statements exist in the tree, and only the strictest is
implemented:

- The language owner: "`dst #= v != dst = #v`. The former will
  **preserve borrow or take ownership**. The latter forces ownership."
- `CAJETA_ERROR_DOUBLE_TRANSFER`: "`#=` already acquires ownership
  **when the source has it** — drop the `#` on the right-hand side."
- `Expression.cpp:113-119`: it "**demands** a title, and a slot without
  one panics `CAJETA_PANIC_TITLE_MISS`."

The first two describe mode carry. The code implements the third.

## 2. The rule

`#` in a **declaration** is a contract — `#T` parameter, `#R` return —
and is statically checkable. `#` at a **use site** (`#x`, `#= x`,
`return #= x`) is a **release**: hand over whatever title this frame
holds, which may be none.

A claim therefore states intent, and the runtime supplies what is
actually there:

| spelling | slot owned | slot borrowed |
|---|---|---|
| `T x = slot` | borrow | borrow |
| `T x #= slot` | **take title** | **borrow** (today: panics) |
| `#slot` into a `#R` return | take title | panic — contract unmet |

## 3. Scope — the three take sites

All in `src/cajeta/asn/expression/Expression.cpp`:

- **3.1** field extraction, ~`:2915` (`xtract_panic`)
- **3.2** sidecar array take, ~`:3008` (`slot_take_panic`), via
  `__cajeta_class_array_elem_take`
- **3.3** tail-bitmap take, ~`:3145` — sidecar-less arrays (fields,
  params)

Each must yield the borrow and a runtime flag of 0 on a miss, **except**
when the consuming site is a declared `#R` return or `#T` argument,
where the contract cannot be honored and the panic stands.

## 4. Why a plain read is not a substitute

`Heap.pop` cannot read the slot plainly and return it: the sift
overwrites `data[0]` with the last element while the heap still holds
the original's title. The title has to leave the slot **with** the
value, which is what the claim does. Any fix that avoids the claim
reintroduces a leak.

## 5. Requirements

- **5.1** `T x #= slot` on a **borrowed** slot yields a borrow. No
  panic, no title minted, and the true owner still drops exactly once.
- **5.2** `T x #= slot` on an **owned** slot is unchanged: the title
  moves to `x`, the slot's bit decays, the slot stays resident and
  readable.
- **5.3** The local's drop entry is armed **iff** the claim took a
  title, so scope exit drops exactly what was taken.
- **5.4** `#slot` consumed by a declared `#R` return or `#T` argument
  still panics `CAJETA_PANIC_TITLE_MISS` on a titleless slot — the
  contract cannot be met and silence would hand out a forged title.
- **5.5** `Heap.pop`, `HashMap.remove` and `LinkedList.popHead` return a
  borrow for a lent element and a title for a transferred one, via
  `return #= x` (already landed).
- **5.6** No OBSERVABLE behaviour change for a workload in which every
  element is owned, measured against a named baseline: the branch tip
  immediately before Unit 1 lands (45dcde99 unless the plan records a later
  one). "Observable" means the pass/fail set of the owned-path pins and
  the drop counts they record — NOT the emitted IR, which necessarily
  differs (a phi and a branch replace a panic block).
  The baseline is deliberately NOT the pre-reversal tree: collections
  borrowing by default is an intended change, so comparing against it
  would flag the intent as a regression.

## 6. Use cases

- **6.1** Lend into a Heap, pop it, read it, let the owner drop:
  exactly one drop, no panic.
- **6.2** Transfer into a Heap, pop it into `T v #= h.pop()`: the caller
  now owns it; one drop at the caller's scope exit.
- **6.3** Mixed: a collection holding one lent and one transferred
  element drops only the transferred one at teardown.
- **6.4** `ArrayList.operator#[]` on a borrowed slot still panics.
- **6.5** A `#R` method that claims a possibly-borrowed slot and returns
  it still panics rather than fabricating a title.

## 7. Non-goals

- **7.1** The `?T` return type (a third static mode). Considered and
  deferred — it collides with PECS wildcards at
  `CajetaParser.g4:422` and reads badly (`?ArrayList<? extends B>`).
  `return #=` covers the need; both funnel through the same flag, so a
  later migration is mechanical.
- **7.2** Diagnosing a short-lived borrow stored in a longer-lived
  collection. Still undiagnosed, still an ordinary lifetime error.

## 8. Regression risk

The claim is the single most load-bearing primitive in the title system
— every container store, extraction and teardown walks through it.

The mitigation is §5.6, and its value depends entirely on the baseline
being RECORDED rather than remembered: capture the owned-path pin results
and their drop counts at 45dcde99 before touching a take site, then diff
after each unit. Any change in that set is a stop.

This criterion is an authoring judgement, not an inherited project
standard — it is written down here so it can be argued with rather than
assumed.
