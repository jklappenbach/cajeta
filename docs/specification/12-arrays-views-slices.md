# 12 — Arrays, Views & Slices

This chapter defines the three bulk-data forms: arrays, the indexed storage type; `view` types, zero-copy overlays that read and write a byte buffer in a declared wire layout; and slices, values that share an immutable backing buffer through the `shared` ownership state.

## 12.1 Arrays

`T[]` is an array of `T`. An array is created with an allocation expression naming its length — `heap int32[3]` — and reports its length through `count()`. Elements are read and written by index.

Index access is bounds-checked at run time: an out-of-bounds access reports the index and the dimension size and terminates the program. It is not an exception a program can catch.

An element store participates in ownership: a plain store lends into the slot, `#=` passes the source's title into it, and the slot records the arrived mode in its own ownership bit (Ownership §5.3). Storing a plain formal into an element is rejected like the field case (Ownership §5.6).

**Example 12.1-1.** Creation, length, and indexing.

```cajeta
public final class C {
    public static int32 run() {
        int32[] a = heap int32[3];
        a[0] = 10; a[1] = 20; a[2] = 30;
        return a[0] + (int32) a.count();    // 13
    }
}
System.stdout.println(C.run());
```

There is no `byte` type; `int8[]` (or `uint8[]`) is the byte buffer (Types §3.1).

## 12.2 Views

A `view` declares a byte-exact overlay onto a buffer: every field's value is encoded directly in the buffer's bytes, at the declared offset, in the declared endianness. Reading a view field reads the buffer; writing one writes it. Nothing is copied, allocated, or owned — a view borrows the buffer it is constructed over.

```text
viewDeclaration
    : VIEW identifier typeParameters?
      classBody
    ;
```

What keeps the bytes-are-the-value guarantee:

- A view field is a primitive, a `String`, an array of primitives, or a nested view — never a class reference: a pointer in untrusted bytes is a wild pointer.
- A view implements no interfaces, inherits from nothing, and has no virtual methods and no vtable.
- Layout is declared, not compiler-chosen: fields lay out in declaration order, packed by default; `@Align(natural)` opts into ABI-natural padding. Endianness is declared with `@BigEndian`, `@LittleEndian`, or `@HostEndian` (Annotations §10.3).
- `String` and array fields are variable-size and lay out inline as a length prefix plus data; fields after one have their offsets resolved once at construction and cached, so every access remains a constant-offset read.

A view is constructed by calling its name with the buffer: `Header h = Header(buf)`. Construction verifies the buffer covers the fixed prefix and that every variable-size length prefix stays in bounds, throwing a parse error otherwise — after construction, field access needs no further validation.

**Example 12.2-1.** A declared wire layout, written and read back.

```cajeta
import cajeta.wire.BigEndian;
@BigEndian
view Header {
    int32 magic;
    int16 version;
}
public final class C {
    public static int32 run() {
        int8[] buf = heap int8[6];
        Header h = Header(buf);          // borrow — buf keeps ownership
        h.magic = 7;
        h.version = (int16) 2;
        return h.magic * 10 + h.version; // 72
    }
}
System.stdout.println(C.run());
```

## 12.3 Slices and the Shared State

A slice is a value that designates a range of another value's immutable backing buffer — `String.substring` is the canonical producer. A borrow cannot express a slice that outlives its source, and forcing a copy would tax the common case; the `shared` ownership state (Ownership §5.1) exists for exactly this.

- **Escaping-borrow resolution.** When a borrow of an eligible source escapes its frame — returned, stored beyond the source's life — it does not error (the identity-object discipline of Ownership §5.6 does not apply): it resolves into a copy for small values, a shared stake in the backing buffer for large ones, and a copy for arena-backed ones.
- **Eligibility** is immutable leaf buffers only — values with no identity, no mutation, and no outgoing references. The shared graph is therefore acyclic: no cycles, no weak references, no leaks.
- **Promotion is one-way**, owned to shared. Moves of a shared value are count-neutral; the count lives in a side table keyed by buffer base, and a buffer that is never sliced-and-stored pays one predicted bit test at drop and nothing else. The last stake frees the buffer.

**Example 12.3-1.** A substring escaping its source's frame.

```cajeta
public final class C {
    public static String tail() {
        String s = "hello world";
        return s.substring(6, 11);   // escapes — resolves via copy or shared stake
    }
}
System.stdout.println(C.tail());     // world
```

> *Discussion.* `Slice<T>` — the generalization of the mechanism beyond `String` — is designed but not shipped; when it lands, this section governs it with `String` as one producer among several.
