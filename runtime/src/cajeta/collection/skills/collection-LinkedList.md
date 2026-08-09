---
id: collection-LinkedList
applies-to: [cajeta/collection/LinkedList]
title: LinkedList — doubly-linked deque for any T
description: Using cajeta.collection.LinkedList as a list/stack/deque; works for primitive and class T because it only needs ==.
---

# LinkedList&lt;T&gt;

Doubly-linked list of `T` in `cajeta.collection`, usable as a list, stack, or
**deque** (O(1) push/pop at either end). Reach for it when you need order-preserving
insertion plus end access, and when `T` is a **primitive** — unlike `HashMap`/`HashSet`
(which need `hash()` and so restrict to class `T` in v1), `LinkedList` only needs `==`
for `contains`/`remove`, so both primitive `T` (built-in `==`) and class `T` (identity
`==`) work. If you need key→value lookup use `HashMap`; for sorted keys see
`collection-ordered-trees-component`; for contiguous indexable storage use `ArrayList`.

This is the **entry point** you instantiate directly. `LinkedListNode<T>` is an internal
support type — the list heap-allocates and links nodes itself; you never construct one.

## Construct

```cajeta
import cajeta.collection.LinkedList;

LinkedList<int32> ll = heap LinkedList<int32>();   // empty; primitive T is fine
ll.add(7);
ll.add(11);
ll.add(13);
boolean has = ll.contains(11);          // true
int32 n = (int32) ll.count();           // 3   (count() returns int64)
```

Two constructors: the no-arg one above, and `LinkedList(#T[] items)` for an array
literal (`heap LinkedList<int32>([1, 2, 3])` — the literal is consumed). The list
**owns its nodes**; it never owns a **value**. `ll.add(v)` lends: the caller keeps
title, and removing or popping never drops the value.

**Do not write `ll.add(#v)`.** The add methods take a plain `T` and hand it on to
`heap LinkedListNode<T>(value)` *plainly*, so a transferred title never reaches the
node — it stays in `add`'s frame and its formal drop frees the value at return,
leaving the node pointing at freed storage (measured: the element's destructor has
already run on the next statement and `head()` reads garbage, while the lent form
reads back intact). Lend, and keep the value alive for as long as the list holds it.

## The methods that matter

- `add(T)` / `addTail(T)` — append at tail, O(1). (`addTail` is a deque-named synonym.)
- `addFirst(T)` / `addHead(T)` — prepend at head, O(1). (synonyms.)
- `head()` / `tail()` → `T` — **peek** an end without removing, O(1).
- `popHead()` / `popTail()` → `T` — remove and return an end, O(1).
- `get(int64 idx)` → `T` — value at 0-based index, O(n) (walks from the nearer end).
- `remove(T)` → `boolean` — unlink first `==`-match; `true` iff something was unlinked, O(n).
- `contains(T)` → `boolean`, O(n).
- `count()` → `int64`.

The add formals are plain `T` and the list always lends — the node stores a borrow and
the caller stays the owner (see Construct for why `#` at the call site is not the
owning spelling here). `popHead`/`popTail` unlink the node and hand the value back as
the borrow it was, so a popped value is only as alive as the local you added.
The array-literal constructor `LinkedList(#T[] items)` does take a `#`: it consumes its
literal.

**`popHead`/`popTail` are primitive-`T` only today.** Their remove-shaped return
extracts the title out of the node with `#=`, and on a lent (i.e. every) class-typed
element that extraction finds no title and throws — measured, an uncaught exception
out of `popHead` on a `LinkedList<Box>`, both for a lent and a transferred add.
Primitive `T` pops fine. For class `T`, read with `head`/`tail`/`get` and unlink with
`remove`.

## Miss / empty semantics — read this before trusting a return

On a **miss** — an empty list, or an out-of-range `get` — `head`, `tail`, `popHead`,
`popTail`, and `get` return the **type's zero value** (the stdlib miss convention), not
null and not an exception, and `popHead`/`popTail` leave the list unchanged. That empty
check runs before the title extraction, so the class-`T` pop throw described above is a
*non-empty*-list hazard, never a miss. When the zero value is itself a valid element,
**guard with `count()`** to distinguish "empty" from "popped a real zero":

```cajeta
if (ll.count() > 0) {
    int32 front = ll.popHead();
    // ...use front...
}
```

## Deque usage

```cajeta
import cajeta.collection.LinkedList;

LinkedList<int32> dq = heap LinkedList<int32>();
dq.addTail(2);
dq.addHead(1);            // [1, 2]
dq.addTail(3);            // [1, 2, 3]
int32 f = dq.popHead();   // 1  -> [2, 3]
int32 b = dq.popTail();   // 3  -> [2]
```

Class `T` works identically (`remove`/`contains` match by reference identity):

```cajeta
LinkedList<Tag> tags = heap LinkedList<Tag>();
Tag a = heap Tag(1);
tags.add(a);                       // lends — `a` still owns the Tag
boolean here = tags.contains(a);   // true — same reference
tags.remove(a);                    // unlinks the node; does NOT drop the lent Tag
```

## Lifecycle & concurrency

No `close()`/dispose — the list and its remaining nodes fall off the drop chain at scope
exit. Mutable and **not** thread/fiber-safe; guard external synchronization if shared.

## What it does NOT do

- **No `Optional`-returning variants** (`headOpt`, etc.) and no bounds-checked/throwing
  `get` — both deferred until the `Optional<class T>` field-layout bug is fixed and
  exception-throwing stdlib intrinsics are wired. Today, misses return the zero value.
- `remove(T)` removes only the **first** match, and unlinks the node without dropping
  the value — the caller owns every element, so nothing on the list ever frees one.
- `contains`/`remove` use `==` only — for class `T` that's **identity**, not deep
  equality.
- No iterator/cursor type; traverse by index with `get` (O(n) per call). For a
  primitive `T` a repeated `popHead`/`popTail` drain is the cheaper full walk; for a
  class `T` that path throws, so walk with `get`.

See `LinkedListNode` (internal node) only if reading the implementation; callers never
touch it.
