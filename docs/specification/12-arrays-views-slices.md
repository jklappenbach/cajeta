# 12 — Arrays, Views, Slices & Records

This chapter defines four ways to shape data. Arrays are the indexed storage type. `view` types are zero-copy overlays that read and write a byte buffer in a declared wire layout. Slices are values that co-own an immutable backing buffer through shared stakes. A `record` is a named value-type aggregate of typed fields, and §12.4 sets it against `view`, the form it is most often confused with.

## 12.1 Arrays

`T[]` is an array of `T`. An array is created with an allocation expression naming its length — `heap int32[3]` — and reports its length through `count()`. Elements are read and written by index.

Index access is bounds-checked at run time: an out-of-bounds access reports the index and the dimension size and terminates the program. It is not an exception a program can catch.

An element store participates in ownership: a plain store lends into the slot, `#=` passes the source's title into it, and the slot records the arrived mode in its own ownership bit (Ownership §5.4). Storing a plain formal into an element is rejected like the field case (Ownership §5.7).

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

There is no `byte` type. `int8[]` (or `uint8[]`) is the byte buffer (Types §3.1).

## 12.2 Views

A `view` declares a byte-exact overlay onto a buffer: every field's value is encoded directly in the buffer's bytes, at the declared offset, in the declared endianness. Reading a view field reads the buffer, and writing one writes it. Nothing is copied, allocated, or owned — a view borrows the buffer it is constructed over.

```text
viewDeclaration
    : VIEW identifier typeParameters?
      classBody
    ;
```

What keeps the bytes-are-the-value guarantee:

- A view field is a primitive, a `String`, an array of primitives, or a nested view — never a class reference: a pointer in untrusted bytes is a wild pointer.
- A view implements no interfaces, inherits from nothing, and has no virtual methods and no vtable.
- Layout is declared, not compiler-chosen: fields lay out in declaration order, packed by default, and `@Align(natural)` opts into ABI-natural padding. Endianness is declared with `@BigEndian`, `@LittleEndian`, or `@HostEndian` (Annotations §10.3).
- `String` and array fields are variable-size and lay out inline as a length prefix plus data. Fields after one have their offsets resolved once at construction and cached, so every access remains a constant-offset read.

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

## 12.3 Slices and Shared Stakes

A slice is a value that designates a range of another value's immutable backing buffer — `String.substring` is the canonical producer. A borrow cannot express a slice that outlives its source, and forcing a copy would tax the common case, so the shared stake exists for exactly this. A stake is a property of the buffer, not of any binding (Ownership §5.1) — a runtime count co-owns the buffer, and the binding holding the slice remains a borrow.

- **Escaping-borrow resolution.** When a borrow of an eligible source escapes its frame — returned, stored beyond the source's life — it does not error (the identity-object discipline of Ownership §5.7 does not apply): it resolves into a copy for small values, a shared stake in the backing buffer for large ones, and a copy for arena-backed ones.
- **Eligibility** is immutable leaf buffers only — values with no identity, no mutation, and no outgoing references. The graph of staked buffers is therefore acyclic: no cycles, no weak references, no leaks.
- **Staking is one-way** — a staked buffer never returns to sole ownership, and only immutable leaf buffers are staked, while identity objects and mutable values never are. Moves of a staked value are count-neutral. The count lives in a side table keyed by buffer base, and a buffer that is never sliced-and-stored pays one predicted bit test at drop and nothing else. The last stake frees the buffer.

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

> *Discussion.* `Slice<T>` — the generalization of the mechanism beyond `String` — is designed but not shipped. When it lands, this section governs it with `String` as one producer among several.

## 12.4 Records

A `record` is a named value-type aggregate of typed fields. A record value is the data. Assigning one copies it, there is no reference identity behind it, and it carries no per-instance header. A record has no vtable, so its methods and operators dispatch directly.

```text
recordDeclaration
    : RECORD identifier typeParameters?
      (EXTENDS typeList)?
      (IMPLEMENTS typeList)?
      classBody
    ;
```

A record is constructed with the named aggregate initializer, which binds each field by name and is order-free. Field access is by name, and an unknown field name is a compile-time error rather than a runtime lookup.

**Example 12.4-1.** Construction and field access.

```cajeta
record Point {
    int32 x;
    int32 y;
}
public final class C {
    public static int32 run() {
        Point p = Point { x: 3, y: 4 };
        return p.x * 10 + p.y;      // 34
    }
}
```

**Records are immutable.** Assigning to a field is a compile-time error, `CAJETA_ERROR_RECORD_IMMUTABLE`. An updated value is produced by `with`, which copies the record and replaces the named fields.

**Example 12.4-2.** Copy-with. The original is unchanged, because `q` is a separate value.

```cajeta
record Point {
    int32 x;
    int32 y;
}
public final class C {
    public static int32 run() {
        Point p = Point { x: 3, y: 4 };
        Point q = p.with(y: 9);
        return q.x * 10 + q.y;      // 39
    }
}
```

Two rules keep a record a value.

- **Fields are value types** — a primitive, a `Vector`, another record, or an `@ValueType` class. A field of a heap class type is a compile-time error, `CAJETA_ERROR_VALUE_TYPE`. An identity reference inside a value would defeat the copy.
- **A record implements no interface.** `record R implements I` parses and is rejected with `CAJETA_ERROR_RECORD_IMPLEMENTS`. Interface dispatch needs a vtable and a record has none. A type that must be reached polymorphically is a class (Classes §8).

### 12.4.1 Records and Views

A record and a view both describe a fixed set of typed fields with no vtable, and they exist for opposite purposes. A view names bytes it does not own. A record is the value itself.

| | `view` (§12.2) | `record` |
|---|---|---|
| Storage | none of its own — an overlay on a buffer supplied from outside | the fields are the value, carried by whoever holds it |
| Layout | declared byte-exact, with endianness, because the wire decides it | the compiler's, subject to the value-type rules |
| Writing a field | writes through into the buffer | rejected — `with` produces a new value |
| Lifetime | borrows the buffer, and cannot outlive it | the value's own, copied wherever it goes |
| Construction | `Header(buf)` validates the bytes cover the fields | `Point { x: 3, y: 4 }` supplies the fields |

Parse or emit a wire format with a view. Model a data shape with a record. A view without a buffer means nothing, and a record needs no buffer at all.

Both exclude class-typed fields, for different reasons. In a view, a pointer read out of bytes the program did not write is a wild pointer. In a record, an identity reference would survive a copy that is supposed to be independent.

> *Discussion.* Records reach further than this chapter states. They carry schemas as type arguments (`Table<Tick>`), they support static vtable-free composition through `extends`, and they are the data-modeling primitive the núcleo typed surface stands on. Positional construction, field defaults, and destructuring are recorded as open in `nucleo/records-spec.md` and are not specified here. That document also still describes `record` as unimplemented, which is stale — every rule in this section is enforced by the shipped compiler.
