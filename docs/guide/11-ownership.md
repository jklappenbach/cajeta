# 11 — Ownership & borrowing

Every heap value has exactly one owner at any time. Plain `=` borrows;
the `#` operator transfers ownership. All of it is checked at compile time —
no annotations, no runtime cost.

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
Point c = #a;                 // transfer — c owns now; a is moved
int32 d = c.distSq();
```

After the transfer, `a` is dead. Reading it again is a compile error, not a
runtime surprise:

<!-- snippet: skip -->
```cajeta
Point p = heap Point(7, 24);
Point q = #p;
int32 v = p.x;    // ERROR — CAJETA_ERROR_USE_AFTER_MOVE
```

## Where `#` goes

`#` always marks the **giving** side. It appears in exactly three places:

- **A move expression** — `this.consume(#a)`, `Point c = #a`. The source is
  moved; its drop entry is deactivated.
- **A parameter type** — `void consume(#Point p)`. The callee demands
  ownership; a caller that passes plain `x` gets
  `CAJETA_ERROR_TRANSFER_REQUIRED`.
- **A return type** — `#Point make()`. The callee hands ownership to the
  caller.

It **never** goes on the receiving local. The receiver is declared plainly —
`Point q = this.make();` — because the transfer is already expressed by the
signature or the move expression. One transfer marker per hand-off, on the
side that gives the value up.

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
        Point q = this.make();        // plain declaration receives ownership
        int32 e = q.distSq();
    }
}
```

A plain `T` parameter can also *accept* an offered `#x` — the caller
surrenders ownership and the value drops in the callee's frame. The tour's
[OwnershipDemo](../../samples/tour/src/main/cajeta/tour/lang/OwnershipDemo.cajeta)
uses exactly that shape.

## Drops at scope exit

Owners are reclaimed automatically at the closing `}` of their declaring
block, in reverse declaration order (LIFO). A `throw` unwinds the same chain,
so cleanup runs on the exception path too. A moved-from local's drop entry is
deactivated — no double free is possible. There is no `delete`.

## The borrow checker

The checker is static and scope-based. Beyond use-after-move it rejects:

- **Borrow escape** — returning or storing a borrow that would outlive its
  source (including the stack-local return from chapter 10).
- **Alias-mutation** — writing through a path while a live borrow into it
  exists (e.g. `list.add(...)` inside a `for` iterating `list`).
- **Definite assignment** — reading a local before it is assigned.

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
        String lower = key.toLowerCase();
        int32 len = (int32) lower.count();
        int32 i = 0;
        while (i + n <= len) {
            String g = lower.substring(i, i + n);
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
