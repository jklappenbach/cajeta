# Cajeta Structs
### POD Aggregates: Wire-Format Views + Inline Values — Specification v1

## Purpose

A `struct` in cajeta is a **plain-old-data aggregate**: a named layout of fields, with no vtable, no inheritance, no heap allocation of its own. It has two equally first-class use cases:

1. **Wire-format view over a byte buffer.** High-throughput server applications process fixed-format messages — RPC requests, protocol frames, file-format records — where each message arrives as a byte buffer. Allocating a new object per message (copying each field out, parsing inline strings, building an object graph) is the dominant cost. A struct can be overlaid onto the existing buffer with field accesses lowered to direct loads. No allocation, no copying, no object graph.

2. **Inline value aggregate.** A struct can also live where it's declared — stack-resident as a local variable, in-line as a class field, passed by pointer through methods. Useful when you want a typed tuple with named fields and no heap overhead — coordinate pairs, `Optional<T>` discriminants, iterator state, the result of a "function that wants to return two values."

The same struct *definition* serves both — the difference is at **construction time** (see "Construction modes" below). Safety in the wire-format-view case is guaranteed by the memory model — the view is a borrow of the buffer, and lifetime/aliasing rules apply automatically (see `MemoryModel.md` § Struct views).

---

## `struct` vs `class`

Cajeta has two type kinds for user-defined aggregates:

- **`class`** — managed object. May have a vtable, may inherit, layout is compiler-chosen for execution efficiency. Backed by heap allocation. Field access via the compiler's chosen offsets.
- **`struct`** — POD aggregate. No vtable, no inheritance, layout is **declared** (compiler emits the exact byte offsets the source implies). Can be either a view over a byte buffer or an inline value at the point of declaration. Intended for wire formats, memory-mapped data, FFI, and small named tuples (coordinates, `Optional<T>`, iterator state).

Use `class` for objects with behavior; use `struct` for data with a known shape.

---

## Declaration

```
struct RequestHeader {
    int32 version;
    int64 timestamp;
    int32 payloadLen;
    String name;
}
```

Each field has a declared type. Primitive types lay out as their native LLVM width. `String` and arrays (`T[]`) are variable-size and lay out as inline length-prefix + data (see "Variable-size fields" below).

Methods (static or instance) can be defined on a struct using the same syntax as a class. Constructors are synthesized by the compiler (see "Construction modes").

---

## Construction modes

A struct definition is just a layout. Two construction modes pick which *kind* of storage backs an instance:

### Inline mode — `MyStruct s;`

The struct's bytes live wherever the declaration sits — a stack alloca for a local, a slot in the enclosing struct or class for a field, a pass-by-pointer parameter slot, a return-value sret slot. **No buffer needed**; the compiler allocates the bytes for you.

```
struct Pair {
    int32 a;
    int32 b;
}

Pair p;          // 8 bytes on the stack, zero-initialized
p.a = 7;
p.b = 11;
return p.a + p.b;
```

Inline mode is the "value aggregate" use case. Lifetime is the enclosing scope (locals) or the enclosing object (fields). No borrow tracking of any backing buffer because there is none.

**Constraint**: every field in an inline-mode-constructed struct must be **fixed-size**. The compiler needs a compile-time-known total size to allocate the slot. Variable-size fields (`String`, `T[]`) require from-bytes construction (see below) where the buffer encodes the actual size at runtime.

### From-bytes mode — `MyStruct s = MyStruct.from(buffer);`

The struct's bytes are an existing `byte[]` (or other byte-typed source). The struct is a **typed view** over those bytes — reads/writes go through to the buffer; no copy. Lifetime is bounded by the buffer's lifetime (statically checked — see "Borrow semantics integration"). Variable-size fields are supported because the buffer carries their length-prefixes.

```
@BigEndian
struct RpcHeader {
    int32 magic;
    int16 version;
    int16 messageType;
    int64 messageId;
    int32 payloadLen;
}

byte[] frame = network.read();
RpcHeader h = RpcHeader.from(frame);    // typed view over frame
if (h.magic != 0xDEADBEEF) throw new ProtocolException();
```

`MyStruct.from(buffer)` is the canonical spelling. The direct call form `MyStruct(buffer)` is also accepted (and is what the parser actually produces today); they lower to the same code path. Earlier drafts of this spec also listed `MyStruct.view(buffer)` — that alias has been **removed**; use `from`.

### Picking the mode

| Use case                              | Mode                          |
|---------------------------------------|-------------------------------|
| Parsing a network frame / file record | from-bytes                    |
| Carrying a `(key, value)` pair around | inline                        |
| Implementing `Optional<T>`            | inline                        |
| Iterator state for a `for` loop       | inline                        |
| Mapping over a memory-mapped file     | from-bytes                    |
| Returning multiple values from a fn   | inline                        |
| Coordinates `(x, y)` in a graphics call | inline                      |

The *struct declaration* doesn't pick the mode — the **construction site** does. Most structs will only ever be used one way, but the language doesn't enforce that. A `Pair` can be either, depending on whether the caller writes `Pair p;` or `Pair p = Pair.from(bytes);` (the latter only valid if `Pair`'s fields are fixed-size, which `int32 a; int32 b;` is).

---

## Layout rules

### Primitives

| Type                              | Bytes |
|-----------------------------------|-------|
| `boolean`                         | 1     |
| `int8` / `uint8` / `char`         | 1     |
| `int16` / `uint16`                | 2     |
| `int32` / `uint32`                | 4     |
| `int64` / `uint64`                | 8     |
| `int128` / `uint128`              | 16    |
| `float16`                         | 2     |
| `float32`                         | 4     |
| `float64`                         | 8     |
| `float128`                        | 16    |

Sub-byte float types (`float8e4m3`, `float4e2m1`, etc.) lay out as 1 byte.

### Packing

**Default: packed.** Fields lay out at consecutive byte offsets, with no padding between them. A struct's fixed-prefix size is the sum of its fixed-field sizes.

### Alignment

Default packed layout means fields may be unaligned (an `int32` starting at offset 1, for example). The compiler emits unaligned-access intrinsics by default for portability — essentially free on x86, slight perf cost on ARM, always correct.

**Opt-in natural alignment** via `@Align(natural)`:

```
@Align(natural)
struct LocalRecord { ... }
```

This inserts padding so each field starts on its natural boundary. Total size grows; field access uses standard aligned loads.

Per-struct annotation only; field-level alignment overrides are not supported in v1.

### Endianness

**Default: host order.** Multi-byte primitives are read/written in the platform's native endianness — no byte-swap on access.

**Opt-in big-endian** via `@BigEndian`:

```
@BigEndian
struct RequestHeader { ... }
```

All multi-byte primitive fields are byte-swapped on access. The compiler emits `bswap` intrinsics when the struct's endianness differs from the host's.

`@LittleEndian` is also accepted (no-op on little-endian hosts, byte-swaps on big-endian hosts).

Endianness is per-struct, not per-field.

---

## Variable-size fields

> Applies to **from-bytes mode only**. Inline-mode structs must have all fixed-size fields; the compiler needs a known total size to allocate the stack slot. A struct declared with any variable-size field cannot be constructed inline; it can only be viewed over a buffer that supplies the actual lengths.

`String` and `T[]` are variable-size. Their inline layout is **length-prefix + data**:

- `String`: `i32 length` followed by `length` bytes of UTF-8 content.
- `T[]` where `T` is fixed-size: `i32 length` followed by `length * sizeof(T)` bytes.

Variable-size fields can appear anywhere in the struct. Fields after a variable-size field have offsets resolved at view-construction time (one length-prefix read per variable-size field; subsequent fields then have constant offsets relative to a cached pointer).

**Optimization:** if all variable-size fields happen to be at the end, every fixed field uses a compile-time-constant offset. The compiler detects this layout and emits the fast path automatically.

Arrays of variable-size elements (e.g. `String[]`) are not supported in v1 as inline-struct fields. Heap-allocated arrays remain available.

---

## Nested structs

A struct field may itself be of struct type. Nested structs lay out **inline** — the inner struct's bytes appear directly within the outer struct, at the outer's field offset. There is no pointer or heap indirection.

```
struct Point {
    int32 x;
    int32 y;
}

struct Line {
    Point start;     // bytes 0..8
    Point end;       // bytes 8..16
}
```

Total size of `Line` is 16 bytes — exactly the sum of its nested layouts.

### Fixed-size inner

If every field of the inner struct is fixed-size, the inner contributes a constant block to the outer's layout. All offsets in the outer remain compile-time-constant.

### Variable-size inner

If the inner struct contains a `String`, an array, or another variable-size struct, the inner is itself variable-size. The outer treats it like any other variable-size field: subsequent fields' offsets are resolved at view-construction time. Length-prefix validation recurses — the view constructor sweeps every length-prefix at every nesting level.

### Endianness inheritance

A nested struct **inherits the outer struct's endianness** unless it has its own `@BigEndian` / `@LittleEndian` annotation. The intent of a `@BigEndian` outer is that every multi-byte primitive in the record is big-endian, regardless of how fields are grouped into nested types.

To opt a sub-region into a different byte order, annotate the nested struct's declaration. Mixed-endian records are rare but supported.

### Alignment inheritance

A nested struct **inherits the outer struct's alignment** unless it has its own `@Align(...)` annotation. Packed outer → packed inner. `@Align(natural)` outer → naturally aligned inner.

### Field access through nesting

`outer.inner.field` is a borrow rooted at `outer`'s buffer per the field-path rule (see `MemoryModel.md` § Path-based borrow tracking). For fixed-size inners the chained offset is constant; for variable-size inners the cached offset table from view construction is used.

### Mutation rules

Mutating a leaf field writes through to the buffer:
- `line.start.x = 5` — 4-byte write at `line.start.x`'s computed offset.
- `line.start = newPoint` — **fixed-size inner only**, byte-copies the inner's bytes into the slot. No `#` required for fixed-size structs (they are value-type fields, like primitives).
- `line.start = newPoint` where `Line.start` is **variable-size** — **static error** (size mismatch would shift subsequent fields). Use leaf-field writes or build a new buffer.

### Recursive types forbidden

A struct may not contain itself, directly or transitively. The compiler detects this during layout and rejects the declaration.

```
struct Bad {
    Bad child;     // STATIC ERROR — infinite size
}
```

Recursive shapes need indirection (a pointer/reference type), which is not in v1.

---

## From-bytes construction

For each `struct` type, the compiler synthesizes a from-bytes constructor:

```
MyStruct.from(byte[] data) -> MyStruct       // borrow-view
```

The constructor:

1. **Size check** — verifies `data.size() >= MyStruct.minimumSize` (the fixed-prefix size). If not, throws.
2. **Length-prefix validation** — single sweep over the data confirming every variable-size length-prefix stays within `data.size()`. If any length-prefix overruns, throws.
3. Returns a `MyStruct` value that borrows `data`.

After this, all field accesses are **bounds-check-free**: the constructor has guaranteed every offset lies within the buffer.

The direct call form `MyStruct(data)` is equivalent and is what the parser actually lowers today. `MyStruct.from(data)` is the recommended spelling because it reads as a deliberate construction-mode choice at the call site, distinct from inline declaration (`MyStruct s;`).

> The earlier draft of this spec listed a third alias, `MyStruct.view(data)`. It has been **removed**. Use `from`. The duplication confused the call-site distinction without adding expressiveness.

### Owning variant (additive)

A second constructor takes the buffer by transfer:

```
MyStruct.from(#byte[] data) -> #MyStruct     // owning-view
```

The struct now owns the buffer. When the struct drops (scope exit or move-out), the buffer drops with it. Useful for parse → build → respond flows where one allocation covers the entire request lifecycle.

v1 may ship borrow-view only; the owning variant is purely additive (no codegen rework, only an additional signature).

### Construction failure

Both bounds-check failures and length-prefix-overflow failures throw a parse exception. Reasoning: server parse loops already have an outer try/catch for malformed inputs; exceptions cost nothing on the happy path; null-return forces a check at every callsite.

The exception type is a built-in (TBD when stdlib classes land); it carries the offending buffer's size and the field offset that failed.

---

## Field access

Read and write IR is the same shape in both construction modes — GEP from the struct's base pointer to the field's offset, then load/store. The only difference is where the base pointer comes from: the from-bytes ctor returns a pointer into the buffer; inline declaration produces a pointer to the stack alloca.

### Reads

- Fixed-offset primitive: `load <type>, ptr (base + offset)`.
- With endian mismatch (from-bytes mode only — inline mode is host-order by definition): `bswap` after the load.
- After a variable-size field (from-bytes mode only): the resolved offset is cached at construction time; subsequent reads are constant-offset against the cached pointer.

### Writes

- Fixed-offset primitive: `store <type>, ptr (base + offset)`.
- With endian mismatch: `bswap` before the store.

### Mutation rules

For **inline-mode** structs: no restrictions. The storage belongs to the enclosing scope; all fields are fixed-size; mutation is uniformly safe.

For **from-bytes-mode** structs:

| Operation                          | Allowed?         |
|------------------------------------|------------------|
| `s.intField = 42`                  | Yes              |
| `s.flags[i] = X`                   | Yes              |
| `s.name.charAt(0) = 'X'`           | Yes (in-place)   |
| `s.name = "longer string"`         | **Static error** |
| `s.flags = newArray`               | **Static error** |

Variable-size field reassignment requires a buffer of a different size — not supported in-place. The caller must allocate a new buffer with the desired total size, view it as the struct, and write fields.

---

## Borrow semantics integration

Applies to **from-bytes mode**. Inline-mode structs don't borrow anything — they own their own stack-resident storage, with the same lifetime story as a primitive local.

A from-bytes struct is a **borrow of its backing buffer**. The standard memory-model rules apply:

- `s` borrows `bytes`; `s` cannot outlive `bytes` (static error otherwise — see the view-aliasing escape check in `LocalVariableDeclaration` and `Statement.cpp`'s `CAJETA_ERROR_VIEW_ESCAPE`).
- `s.field` is a borrow rooted at `bytes` (path-based tracking).
- Mutating `bytes` directly while `s` is live → static error (alias-mutation).
- Mutating through `s.field = X` invalidates other live borrows of `bytes`.

Full details in `MemoryModel.md` § Struct views.

---

## Wire-format versioning

The struct definition **is** the wire spec. Adding/reordering fields, changing widths, endianness, or alignment breaks compatibility with existing producers/consumers — silently.

Recommended versioning patterns:

- **Embed a version field** at a known offset (typically the first field) and dispatch to different struct types per version.
- **Use distinct struct types per protocol version** (`RequestHeaderV1`, `RequestHeaderV2`), with conversion routines between them.

No automatic backwards-compatibility — that's a protocol design problem, not a language problem.

---

## Cross-platform considerations

### Unaligned access

x86 handles unaligned loads/stores transparently with negligible cost. ARMv7+ allows them for most types but at a small perf cost; pre-ARMv6 traps. The compiler emits unaligned-access intrinsics by default for portability. If a struct uses `@Align(natural)`, accesses are aligned and use standard load/store.

### Byte order

Network protocols are typically big-endian (network byte order). Use `@BigEndian` on the wire struct. Most file formats are little-endian; use `@LittleEndian` (or rely on host-order default when targeting only little-endian hosts).

### Wide primitives

`int128` / `uint128` / `float128` may not have native hardware support on all targets. The compiler emits software fallbacks where needed.

---

## What this is not

- **Not for self-describing formats.** Wire structs work for fixed-shape protocols. JSON, MessagePack, CBOR need a different approach (parser → DOM, or generated streaming parsers).
- **Not a replacement for `class`.** Use `struct` only for layout-stable POD; `class` for everything with behavior.
- **Not safe across versions.** Field changes are silently breaking. Add explicit versioning.
- **Not suitable for shared mutable state across threads.** Multi-threading isn't in v1; when added, shared-buffer access will need synchronization primitives.

---

## Errors caught statically (summary)

| Error                                              | Caught by                          |
|----------------------------------------------------|------------------------------------|
| From-bytes struct outliving its buffer             | Memory model § scope check (`CAJETA_ERROR_VIEW_ESCAPE`) |
| Mutating the backing buffer while view is live     | Memory model § alias-mutation rule |
| Reassigning a variable-size field through a view   | Wire-format mutation rule          |
| Reading/writing past the end of the buffer         | Construction-time bounds + length-prefix validation (runtime, once per view) |
| Recursive struct definition (direct or transitive) | Layout-cycle detection during type registration |
| Reassigning a variable-size nested-struct field    | Wire-format mutation rule (same as variable-size leaf) |
| Inline-mode declaration of a struct with variable-size fields | Layout pass — needs known total size at the alloca site |

---

## Implementation outline (for the language implementer)

1. **Parser:** recognize `struct` keyword as distinct from `class`. Accept `@Align(...)`, `@BigEndian`, `@LittleEndian` annotations on struct declarations.
2. **Type system:** add `CajetaStruct` (sibling to `CajetaClass`) with explicit layout computation. Compute fixed-prefix size; identify variable-size fields; pre-compute the constant-offset fast path when applicable. Recurse into nested struct fields, inheriting endianness/alignment from the outer unless overridden. Detect layout cycles (recursive struct definitions) during this pass and reject.
3. **From-bytes constructor synthesis:** for each `struct`, emit a function `MyStruct.from(byte[])` returning a borrow view (and accept the direct `MyStruct(byte[])` form as the same lowering). Emit the size-check and length-prefix-validation sweep. The earlier `MyStruct.view(...)` alias is no longer emitted.
4. **Inline-mode construction:** for `MyStruct s;` declarations, allocate a stack slot of the struct's fixed total size, zero-initialize, and treat field reads/writes as GEPs from the slot. Reject the declaration at the layout pass if the struct contains any variable-size field.
5. **Field accessor codegen:** for each field, emit a getter and setter that performs the offset computation, byte-swap if needed, and the load/store. Same shape in both construction modes — the only difference is where the base pointer comes from (buffer for from-bytes, alloca for inline).
6. **Variable-size offset cache (from-bytes only):** at construction time, walk variable-size fields and cache resolved offsets in the view's internal layout. Recurse into nested variable-size structs so every length-prefix at every nesting level is validated and offsets are resolved before the view is returned. For all-fixed structs (including those with all-fixed nested structs), this step is a no-op.
7. **Borrow checker integration (from-bytes only):** treat from-bytes construction as a borrow of the byte array; treat each field access as a path-based borrow. Reject from-bytes structs whose backing buffer goes out of scope before the struct does (see `CAJETA_ERROR_VIEW_ESCAPE`).
8. **Mutation rule enforcement:** at the AST level, reject reassignment of variable-size struct fields (from-bytes mode only — inline mode can't have variable-size fields, so the rule is vacuous there).
9. **Endianness intrinsics:** emit `bswap` only when struct endianness differs from host endianness (resolved at compile time per target triple). Annotations apply identically to both modes; inline-mode endianness is rare but legal.
10. **Calling convention:** struct values pass by pointer at both call sites and return sites (see `Method::generatePrototype`'s `passByPointer` rule). Applies uniformly to inline and from-bytes structs. Returning a struct from a function follows the same pointer-passing convention.

---

## Examples

### Inline-mode: named tuple as a local

```
struct Pair {
    int32 a;
    int32 b;
}

public static int32 sumPair() {
    Pair p;          // 8 bytes on the stack, zero-initialized
    p.a = 7;
    p.b = 11;
    return p.a + p.b;
}
```

No buffer, no view, no heap. `p` is just two int32 slots in the caller's frame; `p.a = 7` is a `store i32 7, ptr <p>+0`.

### Inline-mode: `Optional<T>` as a value-typed sum

```
public struct Optional<T> {
    private boolean present;
    private T       value;

    public static Optional<T> Some(T v) {
        Optional<T> o;
        o.present = true;
        o.value = v;
        return o;
    }
    public static Optional<T> None() {
        Optional<T> o;       // present defaults to false from zero-init
        return o;
    }
    public boolean isSome() { return this.present; }
    public T unwrap()       { return this.value; }
}
```

Each `Optional<T>` is `{ boolean present; T value; }` — one byte plus T-sized payload, on whatever frame holds it. No allocation. Pass-by-pointer to a method follows the same convention any other struct param does.

### Inline-mode: iterator state for a `for` loop

```
public struct ArrayIter<T> {
    private T[]   data;     // borrowed reference to the heap array
    private int64 idx;
    private int64 stop;

    public Optional<T> next() {
        if (this.idx >= this.stop) { return Optional<T>.None(); }
        T value = this.data[this.idx];
        this.idx = this.idx + 1;
        return Optional<T>.Some(value);
    }
}
```

The iterator carries its cursor + a borrow of the array. Inline mode means `arr.iter()` returns an `ArrayIter<T>` value that lives in the calling scope — no per-loop heap allocation. The `for` loop monomorphizes `.next()` against `ArrayIter<T>` and inlines through.

### From-bytes mode: simple fixed-size record (host endian, packed)

```
struct PixelRGBA {
    uint8 r;
    uint8 g;
    uint8 b;
    uint8 a;
}

byte[] frame = readPixels();
PixelRGBA p = PixelRGBA.from(frame.subarray(0, 4));
println(p.r + ", " + p.g + ", " + p.b);
```

### Network protocol header (big-endian)

```
@BigEndian
struct RpcHeader {
    int32 magic;
    int16 version;
    int16 messageType;
    int64 messageId;
    int32 payloadLen;
}

byte[] frame = network.read();
RpcHeader h = RpcHeader.from(frame);
if (h.magic != 0xDEADBEEF) throw new ProtocolException();
dispatch(h.messageType, frame.subarray(20, 20 + h.payloadLen));
```

### From-bytes mode: variable-size fields

```
struct UserRecord {
    int64 id;
    String username;
    String displayName;
    int32[] permissions;
}

byte[] recordBytes = db.read(key);
UserRecord u = UserRecord.from(recordBytes);
println(u.username + " has " + u.permissions.size() + " permissions");
```

The compiler resolves offsets for `displayName` and `permissions` at from-bytes construction by reading the preceding length-prefixes once. `UserRecord` cannot be constructed inline (`UserRecord u;` would be a static error) because its variable-size fields need a buffer to live in.

### From-bytes mode: nested structs

```
@BigEndian
struct Point {
    int32 x;
    int32 y;
}

@BigEndian
struct BoundingBox {
    Point topLeft;       // bytes 0..8   (inherits @BigEndian)
    Point bottomRight;   // bytes 8..16
    int32 padding;       // bytes 16..20
}

byte[] frame = readBox();
BoundingBox box = BoundingBox.from(frame);
println(box.topLeft.x + ", " + box.bottomRight.y);
```

Nested structs inherit endianness and alignment from the outer unless they carry their own annotation. The inner `Point` doesn't need its own `@BigEndian` here — declaring it explicitly is allowed (and required if a sub-region's byte order needs to differ).
