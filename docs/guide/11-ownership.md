# 11 — Ownership & borrowing

Every heap value has exactly one owner at any time. `=` is always a borrow —
ownership stays with the right-hand side. The `#` operator is a passthrough of
whatever the source holds: a transfer when the source owns, a borrow handed
along when it doesn't. The spelling rules are checked at compile time. Whether
a call or a return carries a title can be decided at run time, so a few
ownership bits travel with the value.

```cajeta
public class Point {
    public int32 x;
    public int32 y;

    public Point(int32 x, int32 y) {
        this.x = x;
        this.y = y;
    }

    public int32 distSq() {
        return this.x * this.x + this.y * this.y;
    }
}
```

## Borrow by default, transfer with `#`

```cajeta
Point a = heap Point(7, 24);
Point b = a;                  // borrow — a still owns; b must not outlive a
Point c #= a;                 // passthrough — a owns here, so the title moves to c
int32 d = c.distSq();
```

After the transfer, `a` is demoted to a borrow of the same live instance —
reading it is still legal, but transferring it again is a compile error:

<!-- snippet: skip -->
```cajeta
Point p = heap Point(7, 24);
Point q #= p;
int32 v = p.x;    // legal — `p` is now a BORROW of the same instance
Point r #= p;     // ERROR — CAJETA_ERROR_MOVE_OF_BORROW
```

## Where `#` goes

`#` marks where a title changes hands. It appears in exactly four places:

- **A store** — `Point c #= a`, `this.held #= v`, `this.data[i] #= v`. The
  destination records the source's mode. It takes the title when the source
  owns one, and it records a borrow when the source was lent. `#=` is one
  token: an ownership store cannot be half-written.
- **A move expression** — `this.consume(#a)`, `return #a`, `#this.data[i]`.
  The source is moved; its drop entry is deactivated. This is the spelling at
  call arguments, returns, and slot extractions — none of which are
  assignments.
- **A parameter type** — `void consume(#Point p)`. The callee demands
  ownership; a caller that passes plain `x` gets
  `CAJETA_ERROR_TRANSFER_REQUIRED`. Its opposite is `void watch(^Point p)`,
  which only ever borrows (see [Keeping a value in a field](#keeping-a-value-in-a-field)).
- **A return type** — `#Point make()`. The callee hands ownership to the
  caller.

The first two split by position, and the split is the whole rule: **a store
uses `#=`; everything else uses `#v`.**

It **never** goes in the receiving local's *type* — `#Point q = …` is
`CAJETA_ERROR_TYPE_TRANSFER_RETIRED` ("a local's role comes from its
initializer"). The **binding** does carry a marker when the callee declares a
transfer: a `#T` result must be received with `#=` — `Point q #= this.make();`
— because that is what makes the acquisition visible without opening the
callee. A plain `=` there is `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`
(spec §4.6). A plain-`T` result still binds with a plain `=`, since it carries
whatever mode the callee's frame held.

> **Deprecated:** stores were once spelled `dst = #v`. That still compiles and
> still transfers, but warns and becomes an error in a later release. Write
> `dst #= v`.

```cajeta
public class Owners {
    public int32 consume(#Point p) {
        return p.distSq();    // p is owned here; it drops when consume returns
    }

    public #Point make() {
        return heap Point(3, 4);    // fresh heap value promotes implicitly
    }

    public void run() {
        Point a = heap Point(7, 24);
        int32 d = this.consume(#a);   // a is moved from this line on
        Point q #= this.make();       // `#Point` result — `#=` records the acquisition
        int32 e = q.distSq();
    }
}
```

A plain `T` parameter can also *accept* an offered `#x` — the caller
surrenders ownership and the value drops in the callee's frame. The tour's
[OwnershipDemo](../../samples/tour/src/main/cajeta/tour/lang/OwnershipDemo.cajeta)
uses exactly that shape.

## `#` forwards the arrived mode

Because `#` is a passthrough, wrappers forward ownership without knowing which
mode they were handed. A plain formal's ownership is decided at the **call
site** — `f(x)` lends, `f(#x)` transfers — and inside the callee, `#p` (or
`this.f #= p`) hands along whichever mode actually arrived: a lent value stays
lent, an owned one transfers. Only a value the compiler can see is purely a
borrow — a local borrowing another local, or a borrow returned by a plain
method — refuses the `#` with `CAJETA_ERROR_MOVE_OF_BORROW`: that surrender
would be a lie.

## Keeping a value in a field

A store into a field, an array slot or a static is where a value starts to
outlive the call that produced it. The compiler checks every such store.

1. A parameter kept with `=` must be spelled `^T`. A `^T` parameter is only
   ever borrowed. A caller cannot pass `#x`, a fresh `heap` value, an owned
   `#T` result or a `+` String into it
   (`CAJETA_ERROR_TRANSFER_INTO_BORROW_PARAM`).
2. A `T` or `#T` parameter that is kept is stored with `#=`. This is the sink
   and setter shape. `set(x)` lends and `set(#x)` transfers.
3. An owned local (bound from `heap`, a call, `#=` or a `+`) stored into a
   field or slot that outlives it is stored with `#=`.
4. A value read out of a parameter or out of an owned local (`b.child`) is
   never stored with `=`. For a `T` or `#T` parameter, spell it `^T` so the
   caller keeps the root alive. For either, `#=` takes the title the slot
   held.
5. Strings follow every rule here, like every other type.
6. `=` never moves. Storing an owned local with `=` is an error even at its
   last use. Spell it `#=`.
7. A producer written in place takes the title with `=`. That covers a
   literal, a primitive, `null`, a `heap T(...)`, a call and a String `+`.
   A local binding `T b = a;` lends, and an argument `f(a)` lends.
8. A local holder whose field or slot is stored into counts as a field when
   it escapes. It escapes when it is returned, passed or stored with `#`, or
   stored into a field, a slot, a static or another holder that escapes. A
   holder bound from a parameter, a field read or a call is not the frame's
   own, so a store into it is a field store. A holder declared in an outer
   block that keeps an owned local from an inner block outlives that local,
   so it needs `#=` too. A holder that stays local is exempt.

```cajeta
public class Block {
    public Block child;
    public Block() { }
}
```

```cajeta
public class Holder {
    Block kept;
    Block seen;
    int32 count;

    public void keep(Block b) { this.kept #= b; }     // the caller chooses: lend or transfer
    public void watch(^Block b) { this.seen = b; }    // only ever a borrow
    public void setCount(int32 n) { this.count = n; } // a primitive carries no title
}
```

| Store | Verdict |
|---|---|
| `void f(Block b) { this.b = b; }` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=`, or spell `^Block` |
| `void f(#Block b) { this.b = b; }` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=` |
| `Block l = heap Block(); this.b = l;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=` |
| `void f(Block b) { this.c = b.child; }` | `CAJETA_ERROR_INTERIOR_KEEP_NEEDS_BORROW_PARAM`: spell `^Block`, or use `#=` |
| `Person o = Persons.load(); this.name = o.name;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=` |
| `String w #= s.substring(1, 9); this.v = w;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: use `#=`, even when `w` is not read again |
| `Node n = heap Node(); n.c = p; return #n;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: `n` escapes, use `#=` or spell `^Cell` |
| `Node h = heap Node(); { Cell l = heap Cell(1); h.c = l; } return h.c.n;` | `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`: `h` outlives `l`, use `#=` |
| `void f(^Block b) { this.b = b; }` | correct, the field borrows |
| `Block o = heap Block(); this.c #= o.child;` | correct, the field takes the title the slot held |
| `Node h = heap Node(); h.c = p; return h.c.n;` | correct, `h` stays local |
| `Node h = heap Node(); Cell l = heap Cell(1); h.c = l; return h.c.n;` | correct, `h` stays local in `l`'s block |
| `this.b #= b;` in any of the above | correct |

In the holder rows, `p` is a plain `Cell` parameter, `Node` has a field
`Cell c`, and `Cell` has a field `n`.

Spelling the parameter `#T` is not a fix on its own. A `#T` parameter owns its
argument and frees it at return, so `#T` with `=` still leaves the field
dangling.

A holder that escapes is spelled the same way as a field.

```cajeta
public class Chain {
    public static #Block wrap(Block b) {
        Block h = heap Block();
        h.child #= b;      // h escapes, so this store follows the field rules
        return #h;
    }
}
```

Strings follow the same spellings. A `#=` of an owned window, such as a
`substring` result, moves the window and its shared stake on the root. A lent window
stored with `#=` resolves. It becomes a copy up to 256 bytes and a shared
stake on the root above that. Bytes from the frame arena always copy when
they escape. A String `=` from a borrowed source also resolves this way
today, rather than storing a plain borrow.

When a fresh node is both registered and linked, register it first and link
it with `#=`. The transfer demotes the local, so the link records a borrow.

<!-- snippet: skip -->
```cajeta
this.nodes.add(#node);     // the registry takes the title
this.root #= node;         // node is now a borrow, so the link records a borrow
```

`^` is an error on a primitive or value-type parameter
(`CAJETA_ERROR_BORROW_MARK_ON_VALUE`) and on varargs
(`CAJETA_ERROR_BORROW_MARK_ON_VARARGS`). It never marks a type-parameter
declaration (`class C<^K>` is an error). A parameter typed by a template
parameter may be spelled `^K key`, and the mark does nothing when `K` is a
primitive. An override must spell the same `^` marks as the method it
overrides (`CAJETA_ERROR_BORROW_MARK_MISMATCH`). Moving a `^T` parameter with
`#p` is `CAJETA_ERROR_MOVE_OF_BORROW`.

The compiler cannot see two hazards. When a caller lends `x` to a holder that
outlives `x`, the holder dangles. The lend may reach the holder through a
`^T` parameter, or through a call such as `h.keep(x)` whose callee keeps it.
The compiler catches only the case where the method that owns `x` returns
`h` (`CAJETA_ERROR_DANGLING_LEND`). The rest needs lifetimes the language
does not track. The second hazard is a sibling field
alias. `this.a = this.b` is a legal borrow. Replacing `this.b` afterwards
frees what `this.a` points at, and the compiler does not track that.

## Drops at scope exit

Owners are reclaimed automatically at the closing `}` of their declaring
block, in reverse declaration order (LIFO). A `throw` unwinds the same chain,
so cleanup runs on the exception path too. A moved-from local's drop entry is
deactivated — no double free is possible. There is no `delete`.

An owned temporary has no name and no block of its own. It is a builder, or
any `#T` call result, used as the receiver of a further call. It is freed at
the end of its statement. In an `if`, `while`, `do` or `for` condition it is
freed right after the condition is evaluated, so a loop frees it on every pass.

## The borrow checker

The checker is static and scope-based. Beyond transfer-from-a-borrow
(`CAJETA_ERROR_MOVE_OF_BORROW`) it rejects:

- **Borrow escape** — returning or storing a borrow that would outlive its
  source (including the stack-local return from chapter 10).
- **Alias-mutation** — writing through a path while a live borrow into it
  exists (e.g. `list.add(...)` inside a `for` iterating `list`).
- **Definite assignment** — reading a local before it is assigned.
- **Borrow of a temporary** (`CAJETA_ERROR_BORROW_OF_TEMPORARY`). A plain
  result that reaches an owned temporary can be used inside its statement.
  Binding it to a local, storing it in a field, or returning it is rejected,
  because the temporary is freed when the statement ends. End a builder chain
  with its `build()` in the same statement. Or bind the temporary to a local
  first and configure it in later statements. Or call a method that returns
  an owned `#T` copy.

```cajeta
import cajeta.io.net.Server;
import cajeta.io.net.ServerBuilder;
import cajeta.io.net.TcpStream;

public class Boot {
    public static void handle(TcpStream conn) { }

    public static #Server open() {
        Server s #= Server.builder()
            .bind("127.0.0.1:0")
            .handler((TcpStream c) -> { Boot.handle(c); })
            .build();                            // build() is #Server, so s owns it
        return #s;
    }

    public static #ServerBuilder configure() {
        // ServerBuilder b #= Server.builder().bind("127.0.0.1:0");   // CAJETA_ERROR_BORROW_OF_TEMPORARY
        ServerBuilder b #= Server.builder();    // bind the temporary first
        b.bind("127.0.0.1:0");
        return #b;
    }
}
```

## Slices and the `shared` state

`arr[a:b]` yields a [`Slice<T>`](../stdlib/lang/Slice.md) — three machine
words (buffer, offset, length) windowing the array's storage. No element
copy; indexing is window-relative and bounds-checked; sub-slicing composes
against the root in O(1). `substring()` and `trim()` return windowed strings
the same way ([chapter 13](13-strings.md)).

```cajeta
public class Windows {
    public int32 firstOfMiddle(int32[] xs) {
        Slice<int32> mid = xs[2:5];       // zero-copy window
        int64 n = mid.count();            // 3
        return mid[0];                    // xs[2]
    }
}
```

A slice that stays local is a plain borrow — it dies with its scope, no
bookkeeping. When a slice *escapes* (stored in a field or container), the
compiler resolves the escape: small payloads copy; large buffers promote
the root from **owned** to **shared** — refcounted, freed at the last drop.
The promotion is automatic; `shared` here is an ownership state the
compiler tracks, not something you write (unrelated to the `@Kernel`
placement keyword).

The store-past-scope idiom is transfer: hand the window to the container
with `#`, and the compiler keeps a stake on the root so the view survives
its source's drop.

```cajeta
import cajeta.collection.ArrayList;

public class Grams {
    // Rolling n-grams of a dying local — the views outlive `lower`.
    public #ArrayList<String> grams(String key, int32 n) {
        ArrayList<String> out = heap ArrayList<String>();
        String lower #= key.toLowerCase();
        int32 len = (int32) lower.count();
        int32 i = 0;
        while (i + n <= len) {
            String g #= lower.substring(i, i + n);
            out.add(#g);                  // transfer — g escapes into the list
            i = i + 1;
        }
        return out;                       // root buffer lives until the last view drops
    }
}
```

Spec: [slice-spec](../specification/lang/slice-spec.md).

Details: [MemoryModel](../specification/lang/MemoryModel.md),
[OwnershipTransfer](../specification/lang/OwnershipTransfer.md), and
[FieldOwnership](../specification/lang/FieldOwnership.md) for how fields
own or alias.

Next: [Control flow](12-control-flow.md).
