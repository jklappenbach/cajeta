# Cajeta Wire Formats & Zero-Copy Structs — Specification v1

## Purpose

High-throughput server applications process fixed-format messages — RPC requests, protocol frames, file-format records — where each message arrives as a byte buffer. Allocating a new object per message (copying each field out, parsing inline strings, building an object graph) is the dominant cost in many such services.

Cajeta supports **zero-copy struct views**: a `struct` type can be overlaid onto an existing byte buffer, with field accesses lowered to direct loads against the buffer. No allocation; no copying; no object graph. Safety is guaranteed by the memory model — the view is a borrow of the buffer, and lifetime/aliasing rules apply automatically (see `MemoryModel.md` § Struct views).

---

## `struct` vs `class`

Cajeta has two type kinds for user-defined aggregates:

- **`class`** — managed object. May have a vtable, may inherit, layout is compiler-chosen for execution efficiency. Backed by heap allocation. Field access via the compiler's chosen offsets.
- **`struct`** — POD aggregate. No vtable, no inheritance, layout is **declared** (compiler emits the exact byte offsets the source implies). Constructible as a view over any byte buffer. Intended for wire formats, memory-mapped data, FFI, and other layout-stable use cases.

Use `class` for objects with behavior; use `struct` for data with a known on-the-wire shape.

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

Each field has a declared type. Primitive types lay out as their native LLVM width. `String` and arrays (`T[]`) are variable-size and lay out as inline length-prefix + data (see below).

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

**Opt-in natural alignment** via `@align(natural)`:

```
@align(natural)
struct LocalRecord { ... }
```

This inserts padding so each field starts on its natural boundary. Total size grows; field access uses standard aligned loads.

Per-struct annotation only; field-level alignment overrides are not supported in v1.

### Endianness

**Default: host order.** Multi-byte primitives are read/written in the platform's native endianness — no byte-swap on access.

**Opt-in big-endian** via `@bigendian`:

```
@bigendian
struct RequestHeader { ... }
```

All multi-byte primitive fields are byte-swapped on access. The compiler emits `bswap` intrinsics when the struct's endianness differs from the host's.

`@littleendian` is also accepted (no-op on little-endian hosts, byte-swaps on big-endian hosts).

Endianness is per-struct, not per-field.

---

## Variable-size fields

`String` and `T[]` are variable-size. Their inline layout is **length-prefix + data**:

- `String`: `i32 length` followed by `length` bytes of UTF-8 content.
- `T[]` where `T` is fixed-size: `i32 length` followed by `length * sizeof(T)` bytes.

Variable-size fields can appear anywhere in the struct. Fields after a variable-size field have offsets resolved at view-construction time (one length-prefix read per variable-size field; subsequent fields then have constant offsets relative to a cached pointer).

**Optimization:** if all variable-size fields happen to be at the end, every fixed field uses a compile-time-constant offset. The compiler detects this layout and emits the fast path automatically.

Arrays of variable-size elements (e.g. `String[]`) are not supported in v1 as inline-struct fields. Heap-allocated arrays remain available.

---

## View construction

For each `struct` type, the compiler synthesizes a view constructor:

```
MyStruct(byte[] data) -> MyStruct       // borrow-view
```

The constructor:

1. **Size check** — verifies `data.size() >= MyStruct.minimumSize` (the fixed-prefix size). If not, throws.
2. **Length-prefix validation** — single sweep over the data confirming every variable-size length-prefix stays within `data.size()`. If any length-prefix overruns, throws.
3. Returns a `MyStruct` value that borrows `data`.

After this, all field accesses are **bounds-check-free**: the constructor has guaranteed every offset lies within the buffer.

### Constructor aliases

The compiler also generates these aliases for readability:

```
MyStruct.from(byte[] data)
MyStruct.view(byte[] data)
```

All three call the same underlying function; use whichever reads best at the call site.

### Owning variant (additive)

A second constructor takes the buffer by transfer:

```
MyStruct(#byte[] data) -> #MyStruct     // owning-view
```

The struct now owns the buffer. When the struct drops (scope exit or move-out), the buffer drops with it. Useful for parse → build → respond flows where one allocation covers the entire request lifecycle.

v1 may ship borrow-view only; the owning variant is purely additive (no codegen rework, only an additional signature).

### Construction failure

Both bounds-check failures and length-prefix-overflow failures throw a parse exception. Reasoning: server parse loops already have an outer try/catch for malformed inputs; exceptions cost nothing on the happy path; null-return forces a check at every callsite.

The exception type is a built-in (TBD when stdlib classes land); it carries the offending buffer's size and the field offset that failed.

---

## Field access

### Reads

Field reads are direct loads against the buffer:

- Fixed-offset primitive: `load <type>, ptr (buffer + offset)`.
- With endian mismatch: `bswap` after the load.
- After a variable-size field: the resolved offset is cached at construction time; subsequent reads are constant-offset against the cached pointer.

### Writes

Field writes are direct stores into the buffer:

- Fixed-offset primitive: `store <type>, ptr (buffer + offset)`.
- With endian mismatch: `bswap` before the store.

### Mutation rules

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

A struct view is a **borrow of its backing buffer**. The standard memory-model rules apply:

- `s` borrows `bytes`; `s` cannot outlive `bytes` (static error otherwise).
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

x86 handles unaligned loads/stores transparently with negligible cost. ARMv7+ allows them for most types but at a small perf cost; pre-ARMv6 traps. The compiler emits unaligned-access intrinsics by default for portability. If a struct uses `@align(natural)`, accesses are aligned and use standard load/store.

### Byte order

Network protocols are typically big-endian (network byte order). Use `@bigendian` on the wire struct. Most file formats are little-endian; use `@littleendian` (or rely on host-order default when targeting only little-endian hosts).

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
| Struct view outliving its buffer                   | Memory model § scope check         |
| Mutating the backing buffer while view is live     | Memory model § alias-mutation rule |
| Reassigning a variable-size field through a view   | Wire-format mutation rule          |
| Reading/writing past the end of the buffer         | Construction-time bounds + length-prefix validation (runtime, once per view) |

---

## Implementation outline (for the language implementer)

1. **Parser:** recognize `struct` keyword as distinct from `class`. Accept `@align(...)`, `@bigendian`, `@littleendian` annotations on struct declarations.
2. **Type system:** add `CajetaStruct` (sibling to `CajetaClass`) with explicit layout computation. Compute fixed-prefix size; identify variable-size fields; pre-compute the constant-offset fast path when applicable.
3. **Constructor synthesis:** for each `struct`, emit a function `MyStruct(byte[])` returning a borrow view, plus `.from(...)` and `.view(...)` aliases. Emit the size-check and length-prefix-validation sweep.
4. **Field accessor codegen:** for each field, emit a getter and setter that performs the offset computation, byte-swap if needed, and the load/store.
5. **Variable-size offset cache:** at view-construction time, walk variable-size fields and cache resolved offsets in the view's internal layout. For all-fixed structs, this step is a no-op.
6. **Borrow checker integration:** treat view construction as a borrow of the byte array; treat each field access as a path-based borrow. Reject struct views whose backing buffer goes out of scope before the view does.
7. **Mutation rule enforcement:** at the AST level, reject reassignment of variable-size struct fields.
8. **Endianness intrinsics:** emit `bswap` only when struct endianness differs from host endianness (resolved at compile time per target triple).

---

## Examples

### Simple fixed-size record (host endian, packed)

```
struct PixelRGBA {
    uint8 r;
    uint8 g;
    uint8 b;
    uint8 a;
}

byte[] frame = readPixels();
PixelRGBA p = PixelRGBA(frame.subarray(0, 4));
println(p.r + ", " + p.g + ", " + p.b);
```

### Network protocol header (big-endian)

```
@bigendian
struct RpcHeader {
    int32 magic;
    int16 version;
    int16 messageType;
    int64 messageId;
    int32 payloadLen;
}

byte[] frame = network.read();
RpcHeader h = RpcHeader(frame);
if (h.magic != 0xDEADBEEF) throw new ProtocolException();
dispatch(h.messageType, frame.subarray(20, 20 + h.payloadLen));
```

### Variable-size fields

```
struct UserRecord {
    int64 id;
    String username;
    String displayName;
    int32[] permissions;
}

byte[] recordBytes = db.read(key);
UserRecord u = UserRecord(recordBytes);
println(u.username + " has " + u.permissions.size() + " permissions");
```

The compiler resolves offsets for `displayName` and `permissions` at view-construction by reading the preceding length-prefixes once.
