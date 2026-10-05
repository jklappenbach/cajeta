---
id: language-ownership
applies-to: [cajeta/language/ownership, cajeta/language/borrowing, cajeta/language/slices]
title: "Ownership, borrowing, # transfer, ^ borrow-only parameters, field stores, drops, and slices"
description: "The rules that keep cajeta memory-safe: borrow by default, transfer with #, drop at scope exit, how to keep a value in a field (#= or a ^T parameter), who decides ownership at each position, and the errors you will meet."
keywords: [ownership, borrow, borrowing, transfer, title, move, lend, lifetime, drop, memory, "#=", "^T", borrow-only, field store, setter, keep, use-after-free, double-free]
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
back-pointer, a view handle. `Cache`'s LRU links use it for exactly that.
From a conditional, `dst #= c ? a : b` acts on the arm that runs: that
arm's title moves, a lent arm records a borrow, and the other arm keeps its
title.

## Keeping a value in a field or slot

A store into a field, an array slot or a static is where a value starts to
outlive the call that produced it. The compiler checks every such store
(spec `field-store-ownership`):

1. A parameter kept with `=` must be spelled `^T`. A `^T` parameter is only
   ever borrowed: a caller cannot pass `#x`, a fresh `heap` value, an owned
   `#T` result or a `+` String into it (`CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM`).
2. A `T` or `#T` parameter that is kept is stored with `#=`. That is the sink
   and setter shape: `set(x)` lends, `set(#x)` transfers.
3. An owned local (bound from `heap`, a call, `#=` or a `+`) stored into a
   field or slot that outlives it is stored with `#=`.
4. A value read out of a parameter or out of an owned local (`b.child`) is
   never stored with `=`. For a `T` or `#T` parameter, spell it `^T` so the
   caller keeps the root alive. For either, `#=` takes the title the slot
   held.
5. Strings follow every rule here, like every other type.
6. `=` never moves. There is no last-use move. Storing an owned local with
   `=` is an error even when nothing reads it again. Spell it `#=`.
7. A producer written in place takes the title with `=`: a literal, a
   primitive, `null`, a `heap T(...)`, a call, or a String `+`. A local
   binding `T b = a;` lends, and an argument `f(a)` lends.
8. A local holder whose field or slot is stored into counts as a field when
   it escapes. It escapes when it is returned, passed or stored with `#`, or
   stored into a field, a slot, a static or another holder that escapes. A
   holder bound from a parameter, a field read or a call is not the frame's
   own, so a store into it is a field store. A holder declared in an outer
   block that keeps an owned local from an inner block outlives that local,
   so it needs `#=` too. A holder that stays local is exempt.

```cajeta
public final class Holder {
    Block kept;
    Block seen;
    public void keep(Block b) { this.kept #= b; }   // caller chooses: lend or transfer
    public void watch(^Block b) { this.seen = b; }  // only ever a borrow
}
```

In the rows below, `p` is a plain `Cell` parameter, `Node` has a field
`Cell c`, and `Cell` has a field `n`.

| Store | Verdict |
|---|---|
| `void f(Block b) { this.b = b; }` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=`, or spell `^Block` |
| `void f(#Block b) { this.b = b; }` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=` |
| `Block l = heap Block(); this.b = l;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=` |
| `void f(Node n) { this.c = n.c; }` | `CAJETA_ERROR_INTERIOR_KEEP_NEEDS_BORROW_PARAM`: spell `^Node`, or use `#=` |
| `Person o = Persons.load(); this.name = o.name;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=` |
| `String w #= s.substring(1, 9); this.v = w;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=`, even at `w`'s last use |
| `Node n = heap Node(); n.c = p; return #n;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: `n` escapes, use `#=` or spell `^Cell` |
| `Node n = heap Node(); n.c = p; this.head #= n;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: `n` escapes into a field |
| `void f(^Node q, Cell p) { Node n = q.prev; n.c = p; }` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: `n` is not the frame's own |
| `Node h = heap Node(); { Cell l = heap Cell(1); h.c = l; } return h.c.n;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: `h` outlives `l`, use `#=` |
| `void f(^Block b) { this.b = b; }` | correct, a borrow |
| `Node o = heap Node(); this.c #= o.c;` | correct, the field takes the title the slot held |
| `Node h = heap Node(); h.c = p; return h.c.n;` | correct, `h` stays local |
| `Node h = heap Node(); Cell l = heap Cell(1); h.c = l; return h.c.n;` | correct, `h` stays local in `l`'s block |
| `Node n = heap Node(); n.c #= p; return #n;` | correct |
| `this.b #= b;` from any of the above | correct |

Spelling `#T` on the parameter is not a fix by itself. A `#T` parameter owns
its argument and frees it at return, so `#T` with `=` still dangles.

Strings follow the same spellings. A `#=` of an owned window (a `substring`
or `trim` result) moves the window and its shared stake on the root. A lent
window stored with `#=` resolves to a copy up to 256 bytes and to a shared
stake on the root above that. Bytes from the frame arena always copy when
they escape. A String `=` into a field is a plain borrow and makes no
copy. To keep your own String past its source, write
`this.v #= o.v.clone()`. A String array slot follows the same rule: each
slot carries its own title, so `a[i] = s` borrows and `a[i] #= s` takes
the title or resolves a lent source.

A store that compiles and reads back correctly is NOT evidence it was right.
Before this check, `#String p; this.v = p` compiled to the same IR as `#=`
(a hidden move), and `#Cell p; this.c = p` read reused memory. Neither
compiles now, and no `=` store moves.

When a fresh node is both registered and linked, register it first and link
it with `#=`: the transfer demotes the local, so the link records a borrow.

```cajeta
this.nodes.add(#node);     // the registry takes the title
this.root #= node;         // node is now a borrow; the link records a borrow
```

`^` is an error on a primitive or value-type parameter and on varargs. On a
parameter typed by a template parameter (`void put(^K key)`) it is legal and
does nothing when `K` is a primitive. An override must spell the same `^`
marks as the method it overrides (`CAJETA_ERROR_BORROW_MARK_MISMATCH`).

Two hazards the compiler cannot see:

- A caller that lends `x` to a holder that outlives `x`, through a `^T`
  parameter or a call like `h.keep(x)` whose callee keeps it. Only the case
  where the owning method returns `h` is caught (`CAJETA_ERROR_DANGLING_LEND`).
  The rest needs lifetimes the language does not track.
- A sibling field alias. `this.a = this.b` is a legal borrow. Replacing
  `this.b` afterwards frees what `this.a` points at, and nothing reports it.

## The four places `#` appears — and the one place it never does

- **Store**: `Point c #= a;`, `this.held #= v;`, `this.data[i] #= v;` — the
  destination records the SOURCE'S MODE (a title when one was tendered, a
  borrow otherwise). `#=` is one token.
- **Move expression**: `this.consume(#a)`, `return #a`, `#this.data[i]` — at
  call arguments, returns, and slot extractions (none of these are stores).
- **Parameter type**: `void consume(#Point p)` — the callee demands ownership.
  Its opposite is `void watch(^Point p)` — the callee only ever borrows, and
  a caller cannot transfer into it.
- **Return type**: `#Point make()` — the callee hands ownership out; a fresh
  `heap T(...)` promotes implicitly.

The rule: **a store uses `#=`; everything else uses `#v`.** `#` never goes in
the receiving local's TYPE — `#Point q = …` is
`CAJETA_ERROR_TYPE_TRANSFER_RETIRED`. It DOES go on the BINDING when the callee
declares a transfer: a `#T` result must be received with `#=` —
`Point q #= this.make();` — and a plain `=` there is
`CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER` (spec §4.6), because a reader
otherwise cannot tell an acquisition from a borrow-returning call without
opening the callee. `#T` is not advisory: its flag is a constant 1 (§2.8), the
callee must establish a title at every return (§4.5), and the receiving `#=`
registers a drop unconditionally. A plain-`T` result still binds with a plain
`=`; there the title, *if one is tendered*, arrives on the return flag and arms
`q`'s drop entry — and whether one is tendered really is the callee's runtime
decision (correction 1). (Legacy `dst = #v` still compiles with a deprecation
warning; write `dst #= v`.)

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
- `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE` fires when a `T` or `#T` parameter,
  an owned local, or a value read out of an owned local is kept in a field,
  slot, static or escaping holder with `=`. Store with `#=`, or spell the
  parameter `^T` when it is only ever borrowed.
- `CAJETA_ERROR_INTERIOR_KEEP_NEEDS_BORROW_PARAM` fires when a value read out
  of a `T` or `#T` parameter is kept with `=`. Spell the parameter `^T`, or
  store with `#=`.
- `CAJETA_ERROR_DANGLING_LEND` fires when an owned local is lent to a holder
  through a call that keeps it (`h.keep(s)`) and the holder is returned.
  Pass `#s`.
- `CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM` — passing `#x`, a fresh `heap`
  value, an owned `#T` result or a `+` String to a `^T` parameter: bind it to
  a local and pass the local.
- `CAJETA_ERROR_BORROW_MARK_MISMATCH` — an override whose `^` marks differ
  from the method it overrides.
- `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER` — receiving a `#T` result with a
  plain `=`: spell the binding `x #= f()`.
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
        Point q #= d.make();            // `#Point make()` — the title moves here
        return borrowed + moved + q.distSq();   // 1275
    }
}
```

## The stdlib convention — producer / view / sink (spec §2)

Every stdlib API answers one question: **who may legitimately outlive the
caller's scope?** Only sinks may. Everything else is producing a value or
lending a view, and each genre has one correct spelling
(`specs/stdlib-ownership-convention-spec.md`; inventory + dispositions in
`docs/stdlib/ownership-audit.md` / `ownership-dispositions.md`):

| Genre | Shape | Spelling | Examples |
|---|---|---|---|
| **producer** (§2.1) | materializes a NEW value | returns `#T`; receive with `#=` | `asString`, `toBytes`, `encode` |
| **view** (§2.2) | exposes interior state for reading | plain `T`, body is interior reads only — always a borrow | `keyAt`, `get(i)`, `asBytes` |
| **sink** (§2.3) | container whose job is holding values | plain `T` param + `#=` slot store — the CALLER chooses per call | `ArrayList.add`, node/carrier ctors |
| **keeper** | keeps a parameter beyond the call | `#=` store; a `^T` parameter when it is only ever borrowed (field-store-ownership spec) | every exception's `this.message #= message`, `Cache`'s link helpers (`^`) |
| **copy-vs-alias** (§2.5) | could do either | it COPIES; the alias is a separately-named sharp variant | `setString` / `setStringBorrowed` |

`^T` (§2.8) is the opt-in FORCED-borrow return — body restricted to borrow
sources, checked rather than described. On a parameter, `^T` is the
borrow-only formal described above. Plain `T` stays the default because a
view's frame holds no title: it already carries a borrow.

Ownership conditioned on a runtime property the caller cannot see — SSO vs
sliced representation, which constructor ran — is **non-conforming** (§2.6).
`JsonValue.setString(String)` was the stdlib's only instance and now copies
unconditionally.

**The `keyAt` mistake, worked** (this one is invisible without the example —
it compiled clean and corrupted at a distance):

```cajeta
int8[] kb = o.keyAt(j);          // VIEW — a borrow into o's interior
heap String(#kb, kl);            // WRONG: # on a borrow FORWARDS the mode;
                                 // o still owns, frees on drop → the String
                                 // reads recycled memory (cajeta-llama bug)
```

The fix is to copy at the boundary — take the view, materialize your own
owner from it — or use the producer-shaped accessor when one exists:

```cajeta
int8[] kb = o.keyAt(j);          // borrow, fine to READ while o lives
int8[] mine #= copyOf(kb, kl);   // your own title, safe past o's drop
heap String(#mine, kl);
```

Two lints police the seams the checker cannot reject outright:

- **`[plain-return-yields-title]`** — a plain-`T` method whose every return
  hands out a fresh allocation: the caller receives a title the signature
  never declared. Declare the return `#T`.
- **`[plain-return-of-owned-slot]`** — returning a borrow of a slot the
  frame is about to free. Deliberately unprotected at runtime; the lint is
  the only fence.

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

- **A setter stores with `#=`.** It is safe whichever way the caller passed
  the value: a surrendered value is owned by the field, a lent one is
  borrowed. A plain `=` there is `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`.
  Declaring `#T` is a different, stronger choice: it forces every caller to
  surrender, and the store still needs `#=`.

  ```cajeta
  public Box(T v) { this.value #= v; }   // the field records what arrived
  ```
- Ownership at a call site is directional: a plain `T` parameter can *accept*
  an offered `#x` (the value then drops in the callee), a `#T` parameter
  never accepts a plain borrow, and a `^T` parameter never accepts a transfer.
- **Containers do NOT own their elements by default — the CALLER chooses, per
  call.** A collection sink takes a plain `T` and stores it with `#=`, so
  `list.add(g)` LENDS and `list.add(#g)` transfers, with the arriving mode
  recorded per slot (`ArrayList.add(T v)`, `LinkedList.add`, `HashSet.add`,
  `Heap.push`, `HashMap.put`). Teardown drops exactly the slots whose title was
  tendered, so a lent element is never freed by the container and a surrendered
  one always is. Owning aggregates are a different genre: `Pair(#K, #V)` and
  `HashMap.operator[]=(#K, #V)` really do force transfer, and passing a plain
  owned local there is `CAJETA_ERROR_TRANSFER_REQUIRED`.

  ```cajeta
  list.add(#g);          // the list takes g's title
  int64 n = g.count();   // g is a demoted borrow — still readable
  ```

  You do NOT need to capture what you still want before the add. `#` moves the
  title, not the binding, so `g` reads fine afterwards for as long as the list
  is alive. What you must not do is read it *after the list tears down* — the
  list freed the element, and nothing diagnoses that yet (MemoryModel §1.7).


  ```cajeta
  list.add(#s);                          // surrender the one String
  list.add(s.substring(0, s.count()));   // give the list its own copy; s stays owner
  ```

  Reach for the copy when the caller genuinely needs to keep an owner past the
  container's life; otherwise surrender and read `s` as a borrow.
