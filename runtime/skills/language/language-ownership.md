---
id: language-ownership
applies-to: [cajeta/language/ownership, cajeta/language/borrowing, cajeta/language/slices]
title: Ownership, borrowing, # transfer, drops, and slices
description: The rules that keep cajeta memory-safe at compile time — borrow by default, transfer with #, drop at scope exit — and the borrow-checker errors you will meet.
---

# Ownership & transfer — read this before storing, returning, or passing heap values

Every heap value has exactly one owner. Plain `=` **borrows** (source still
owns; the borrow must not outlive it). `#` **transfers** the title. Everything
is checked at compile time; there is no annotation burden and no runtime cost.

## The four places `#` appears — and the one place it never does

- **Store**: `Point c #= a;`, `this.held #= v;`, `this.data[i] #= v;` — the
  destination takes the title, the source is moved. `#=` is one token.
- **Move expression**: `this.consume(#a)`, `return #a`, `#this.data[i]` — at
  call arguments, returns, and slot extractions (none of these are stores).
- **Parameter type**: `void consume(#Point p)` — the callee demands ownership.
- **Return type**: `#Point make()` — the callee hands ownership out; a fresh
  `heap T(...)` promotes implicitly.

The rule: **a store uses `#=`; everything else uses `#v`.** `#` never goes on
the receiving local's declaration — `Point q = this.make();` is plain, because
the signature already carries the transfer. (Legacy `dst = #v` still compiles
with a deprecation warning; write `dst #= v`.)

## Drops

Owners are reclaimed at their block's closing `}`, in reverse declaration
order, on the normal *and* the exception path. A moved-from local's drop entry
is deactivated — double free is structurally impossible. There is no `delete`.

## The borrow-checker errors you will meet (all verified)

- `CAJETA_ERROR_USE_AFTER_MOVE` — reading `p` after `q #= p` / `f(#p)`:
  "path 'p.x' was transferred via `#` and cannot be read here."
- `CAJETA_ERROR_TRANSFER_REQUIRED` — passing plain `a` where the parameter is
  `#T`: write `#a`, or pass a fresh `heap T(...)` construction.
- `CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER` — returning an owned local
  through a non-`#` return type (would silently leak): mark the return `#T`.
- **Borrow escape** — returning/storing a borrow that outlives its source.
- **Alias-mutation** — mutating a container while a live borrow into it
  exists (e.g. `list.add(...)` inside a `for` over `list`).

## Worked example (verified: returns 1275)

```cajeta
package dev.cajeta.skills;

public class OwnershipDemo {
    public int32 consume(#Point p) {
        return p.distSq();          // p is owned here; drops when consume returns
    }

    public #Point make() {
        return heap Point(3, 4);    // fresh heap value promotes implicitly
    }

    public static int32 run() {
        OwnershipDemo d = stack OwnershipDemo();
        Point a = heap Point(7, 24);
        Point b = a;                    // borrow — a still owns
        int32 borrowed = b.distSq();    // 625
        Point c #= a;                   // transfer store — c owns; a is moved
        int32 moved = d.consume(#c);    // move expression at the call site
        Point q = d.make();             // plain declaration receives ownership
        return borrowed + moved + q.distSq();   // 1275
    }
}
```

## Slices and the `shared` state

`arr[a:b]` yields a zero-copy `Slice<T>` window (buffer, offset, length);
`substring()`/`trim()` window strings the same way. A local slice is a plain
borrow. When a view *escapes* (stored past its source's scope), hand it over
with `#` — `out.add(#g)` — and the compiler resolves the escape: small
payloads copy, large buffers promote the root to a refcounted **shared** state
freed at the last drop. The promotion is automatic; `shared` here is a tracked
ownership state, not something you write (unrelated to the `@Kernel`
placement keyword).

## Sharp edges

- **Returning a `stack` value through a `#` return type is rejected** —
  `CAJETA_ERROR_STACK_RETURN_ESCAPES`, for both `return stack X(...)` and
  `Cell c = stack Cell(); return #c;`. Anything that escapes a frame must be
  `heap`. (This was silent UB before 2026-07-31.)
- **Storing a fresh value into a field with a plain `=` dangles.**
  `Box(T v) { this.value = v; }` called as `heap Box(heap Cell(1))` reads freed
  memory. Two rules compose, and both are working as designed: `heap X(...)` at
  a CALL SITE surrenders, so the formal `v` becomes the owner; and `=` is a
  borrow that never inherits that contract. The formal still owns at return, so
  its drop fires and the field is left pointing at freed memory. The program
  asked to borrow from a value whose owner dies at the end of the call.

  **The fix is `#=` at the store — the formal stays plain:**

  ```cajeta
  public Box(T v) { this.value #= v; }   // field takes the title
  ```

  `#=` consumes the formal's title, so its drop is deactivated. It is safe
  whichever way the caller passed the value — surrendering
  (`heap Box(heap Cell(1))`) and lending (`Cell c = heap Cell(1); heap Box(c);`)
  both work — so you can apply this at the store site without reasoning about
  callers.

  Declaring `#T` is a *different*, stronger choice: it is API-visible and forces
  every caller to surrender. Reach for it when you want to REQUIRE ownership,
  not to fix this — changing the store is enough.

  Passing a named local with a plain `=` store also stays correct: that lends,
  and the field aliases a value the caller still owns.
  (`specs/field-store-title-trap-spec.md`.)
- Ownership at a call site is directional: a plain `T` parameter can *accept*
  an offered `#x` (the value then drops in the callee) — but a `#T` parameter
  never accepts a plain borrow.
- Containers take elements by transfer (`list.add(#g)`); after the add, the
  local is moved — capture anything you still need (e.g. `count()`) first.
