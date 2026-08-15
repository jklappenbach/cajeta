---
id: language-ownership
applies-to: [cajeta/language/ownership, cajeta/language/borrowing, cajeta/language/slices]
title: Ownership, borrowing, # transfer, drops, and slices
description: The rules that keep cajeta memory-safe — borrow by default, transfer with #, drop at scope exit — WHO decides ownership at each position, and the borrow-checker errors you will meet.
keywords: [ownership, borrow, borrowing, transfer, title, move, lend, lifetime, drop, memory, "#=", use-after-free, double-free]
---

# Ownership & transfer — read this before storing, returning, or passing heap values

Every heap value has exactly one owner.

**Ownership is runtime-conditional on BOTH sides of a call.** What differs by
position is *who decides*:

| Position | Decided by | Carried in | Spelling |
|---|---|---|---|
| name → name | the **spelling** | statically | `=` lends, `#=` transfers |
| call argument | the **caller** | the transfer word | `f(x)` lends, `f(#x)` transfers |
| return | the **callee** | the return-flag TLS | plain `T` may STILL carry a title |
| slot store | the **source's mode** | per-slot bit, via `#=` | a lend stays a lend |

Only the first row is the simple "`=` lends, `#` transfers" rule. **Do not
reason from that rule alone.** An earlier revision of this skill said
ownership was "checked at compile time … no runtime cost"; that is wrong in
three ways, each of which produced a real defect. Every correction below was
measured, with the test named.

## Three things the simple rule gets wrong

**1. A plain (non-`#`) return is NOT statically a borrow.** A plain-return
wrapper that tail-calls a `#` method rides the inner title through:

```cajeta
public static #Cell fresh()    { return heap Cell(7); }
public static Cell  viaPlain() { return D.fresh(); }   // returns a TITLE
```

[`SignatureAbiTests.tailCallThroughPlainReturnKeepsTitle`. `Stream.fold<R>`
does the same via its callback's `#R` — genuinely runtime-variable, since the
callback is a parameter.] So `T x = someCall()` is not a lend: the local's
drop entry is armed from the arriving flag.

**2. `#x` on a borrow does NOT transfer — it FORWARDS the mode it was
handed.** The lender keeps title and frees on drop, so a receiver that
outlives the lender reads reused memory. Measured, both kinds: an array
payload read back `-83968` instead of `8247`; a class payload came back
holding the next allocation's bytes [`OwnershipArrayCanaryTests`]. That is a
use-after-free, not a style issue.

**3. `#=` is MODE-CARRYING — it is not a transfer.** It records whatever mode
the source actually holds, so a **lent source records a BORROW** and is not
moved. It makes no claim of title, is therefore always safe, and is the
correct spelling for a deliberate non-owning alias — an intrusive link, a
back-pointer, a view handle. `Cache`'s LRU links and `Channel`'s slots both
use it for exactly that.

## The four places `#` appears — and the one place it never does

- **Store**: `Point c #= a;`, `this.held #= v;`, `this.data[i] #= v;` — the
  destination records the SOURCE'S MODE (a title when one was tendered, a
  borrow otherwise). `#=` is one token.
- **Move expression**: `this.consume(#a)`, `return #a`, `#this.data[i]` — at
  call arguments, returns, and slot extractions (none of these are stores).
- **Parameter type**: `void consume(#Point p)` — the callee demands ownership.
- **Return type**: `#Point make()` — the callee hands ownership out; a fresh
  `heap T(...)` promotes implicitly.

The rule: **a store uses `#=`; everything else uses `#v`.** `#` never goes on
the receiving local's declaration — `Point q = this.make();` is plain, and the
title (if one is tendered) arrives on the return flag and arms `q`'s drop
entry. Note the parenthetical: whether a title IS tendered is the callee's
runtime decision, not something the return spelling guarantees — see
correction 1. Treat `#T` on a return as the PRODUCER/VIEW contract it is
meant to be, and verify rather than assume when it matters. (Legacy
`dst = #v` still compiles with a deprecation warning; write `dst #= v`.)

**Never both.** `x #= #y` is `CAJETA_ERROR_DOUBLE_TRANSFER` whatever `y` is —
identifier, field, element, or call result. The store carries the transfer, so
the second `#` says nothing the first did not. There is no source shape that
takes both, and no exception to memorize.

## Drops

Owners are reclaimed at their block's closing `}`, in reverse declaration
order, on the normal *and* the exception path. A transferred local is DEMOTED to a borrow: its drop entry is deactivated, so
double free is structurally impossible, and the binding stays readable. There is no `delete`.

## The borrow-checker errors you will meet (all verified)

- `CAJETA_ERROR_MOVE_OF_BORROW` — transferring from something that does not
  own its value. A transfer DEMOTES its source to a borrow, so transferring
  twice raises this too: "You cannot transfer ownership more than once, or
  from a borrow." Reading `p` after `q #= p` is NOT an error — `p` is a
  readable borrow of the same live instance.
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
- **Containers OWN their elements, and the rule is enforced, not conventional.**
  Every stdlib container declares its element parameters `#T`, so lending one a
  plain local is `CAJETA_ERROR_TRANSFER_REQUIRED` at the call site, naming the
  fix. There is no borrowed-element mode: `list.add(g)` does not compile, and a
  container never holds something it will not reclaim.

  ```cajeta
  list.add(#g);          // the list takes g's title
  int64 n = g.count();   // g is a demoted borrow — still readable
  ```

  You do NOT need to capture what you still want before the add. `#` moves the
  title, not the binding, so `g` reads fine afterwards for as long as the list
  is alive. What you must not do is read it *after the list tears down* — the
  list freed the element, and nothing diagnoses that yet (MemoryModel §1.7).

- **String is a normal owned class here.** `list.add(s)` on a `String` is the
  same error as any other element, which is the single largest source of
  migration churn. Two fixes, and they mean different things:

  ```cajeta
  list.add(#s);                          // surrender the one String
  list.add(s.substring(0, s.count()));   // give the list its own copy; s stays owner
  ```

  Reach for the copy when the caller genuinely needs to keep an owner past the
  container's life; otherwise surrender and read `s` as a borrow.
