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
**owns its nodes**, and owns a **value** exactly when you tender one: the node's ctor
stores with `#=` (`LinkedListNode.cajeta:46`), so the mode you spell at the call site is
recorded per node. This is the §2.3 sink model, the one genre where the developer chooses:

- `ll.add(v)` **lends** — the caller keeps title, and removing or popping never drops the
  value. Keep the value alive for as long as the list holds it.
- `ll.add(#v)` **transfers** — the node owns that value, list teardown reclaims it through
  the node chain, and a pop hands the title back out.

`add`/`addFirst`/`addTail`/`addHead` forward internally with `#value`
(`LinkedList.cajeta:97, 105, 117, 143`), which is what carries your mode through to the
node. A wrapper of your own that forwards a **plain** formal on to `add` does not pass the
title along — it drops at the wrapper's return and leaves the list holding freed storage —
so spell `#` on the forward, as the stdlib does.

## The methods that matter

- `add(T)` / `addTail(T)` — append at tail, O(1). (`addTail` is a deque-named synonym.)
- `addFirst(T)` / `addHead(T)` — prepend at head, O(1). (synonyms.)
- `head()` / `tail()` → `T` — **peek** an end without removing, O(1).
- `popHead()` / `popTail()` → `T` — remove and return an end, O(1).
- `get(int64 idx)` → `T` — value at 0-based index, O(n) (walks from the nearer end).
- `remove(T)` → `boolean` — unlink first `==`-match; `true` iff something was unlinked, O(n).
- `contains(T)` → `boolean`, O(n).
- `count()` → `int64`.

The add formals are plain `T` and the node stores with `#=`, so the caller chooses per
call: plain lends and the caller stays the owner, `#` transfers and the node owns the slot
(see Construct). `popHead`/`popTail` unlink the node and return the value in the mode the
node held it — a lent element comes back a borrow, only as alive as the local you added,
while a transferred one hands its title out to the receiving local.
The array-literal constructor `LinkedList(#T[] items)` does take a `#`: it consumes its
literal.

**`popHead`/`popTail` are remove-shaped, flagged returns (spec §2.8 transparent carry).**
Both are declared plain `T` and their bodies forward whatever mode the node recorded —
`T title #= node.value; return #= title;`. An element added with `#v` hands its title back
to your receiving local; one added by lending comes back as a borrow. Receive with plain
`=`: the result is `T`, not `#T`. Class `T` works — `LinkedList<String>` pops are pinned by
`LinkedListClassPopTests`; the earlier forced-title panic was fixed in `64b7b388`.

## Miss / empty semantics — read this before trusting a return

On a **miss** — an empty list, or an out-of-range `get` — `head`, `tail`, `popHead`,
`popTail`, and `get` return the **type's zero value** (the stdlib miss convention), not
null and not an exception, and `popHead`/`popTail` leave the list unchanged. That empty
check runs before the title extraction, so a miss never reaches the node. When the zero
value is itself a valid element,
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
- `remove(T)` removes only the **first** match, and drops the node; the value goes with it
  only if you transferred it — a lent element is untouched and stays the caller's.
- `contains`/`remove` use `==` only — for class `T` that's **identity**, not deep
  equality.
- No iterator/cursor type; traverse by index with `get` (O(n) per call). For a `T` of any
  kind a repeated `popHead`/`popTail` drain is the cheaper full walk (it consumes the
  list); `get` is the non-destructive one.

See `LinkedListNode` (internal node) only if reading the implementation; callers never
touch it.
