# Cajeta Views — Specification v1

## Purpose

A `view` is a **typed overlay onto a byte buffer**. Reads and writes go through to the buffer's bytes directly; no allocation, no copy, no object graph. It is the right choice for:

- **Network protocol frames** — RPC requests, packet headers, framing layers (TCP / IP / custom).
- **Memory-mapped file records** — fixed-layout binary formats, database pages, on-disk record types.
- **Zero-copy serialization formats** — Cap'n Proto data sections, FlatBuffers (with caveats), custom binary protocols.

A `view` declaration names a layout — a sequence of fields with declared byte positions, endianness, and packing. The layout exists as a type. An *instance* of a view is the layout plus a borrowed byte buffer. Constructing the instance is bounds-checking the buffer and producing a typed handle; per-field access is then a direct GEP + load/store with no further runtime cost.

Views are **not** for self-describing formats — Ion, JSON, CBOR, MessagePack, BSON, Protobuf. Those have variable-length, tag-encoded structure where offsets depend on the data; they need streaming parsers (see `cajeta.codec.*` libraries), not overlay. The line: **if field offsets are determined by the type alone, view works. If they depend on the data, you need a parser.**

Views are not for value aggregates with class references or behavior. That is the unified `class` keyword's job — see `cajeta-docs/stdlib/UnifiedClasses.md`. (`struct` is a transitional alias for `class`; the legacy stand-alone struct design doc has been retired.)

---

## `view` vs `struct` vs `class`

| | `class` | `struct` | `view` |
|---|---|---|---|
| Storage | heap | stack alloca | borrowed bytes |
| Lifetime | ownership-tracked | enclosing scope | borrowed buffer |
| Class refs in fields | yes | yes | **no** |
| Layout | compiler-chosen | compiler-chosen | declared, byte-exact |
| Endianness | host | host | **annotation-required** |
| Allocation on construction | yes (`new`) | no (stack) | no (borrows / takes buffer) |
| Vtable | yes (header) | no | no |
| Implements interfaces | yes | yes | no |

The defining constraint of `view`: every field's value is encoded *directly* in the buffer's bytes. Class references can't be in a view because a pointer in untrusted bytes is a wild pointer; reading through it would crash or return garbage.

---

## Declaration

```cajeta
@BigEndian
view RpcHeader {
    int32  magic;
    int16  version;
    int16  messageType;
    int64  messageId;
    int32  payloadLen;
}
```

Each field has a declared type. Primitive types lay out as their native LLVM width. `String` and array fields (`T[]`) are variable-size and lay out as inline length-prefix + data (see "Variable-size fields"). Nested view types lay out inline (see "Nested views").

Methods can be declared on a view using the same syntax as a class or struct, with restrictions documented under "Methods" below.

Views **cannot**:

- Hold class references as fields.
- Implement interfaces.
- Inherit from other views.
- Have virtual methods.

These constraints aren't arbitrary; each is necessary for the "bytes in the buffer are the value" guarantee.

---

## Layout rules

### Primitives

| Type | Bytes |
|---|---|
| `boolean` | 1 |
| `int8` / `uint8` / `char` | 1 |
| `int16` / `uint16` | 2 |
| `int32` / `uint32` | 4 |
| `int64` / `uint64` | 8 |
| `int128` / `uint128` | 16 |
| `float16` | 2 |
| `float32` | 4 |
| `float64` | 8 |
| `float128` | 16 |

Sub-byte float types (`float8e4m3`, `float4e2m1`, etc.) lay out as 1 byte.

### Packing

**Default: packed.** Fields lay out at consecutive byte offsets with no padding between them. A view's fixed-prefix size is the sum of its fixed-field sizes.

### Alignment

Default packed layout means fields may be unaligned (an `int32` starting at offset 1, for example). The compiler emits unaligned-access intrinsics by default — essentially free on x86, small perf cost on ARM, always correct.

**Opt-in natural alignment** via `@Align(natural)`:

```cajeta
@Align(natural)
view LocalRecord { ... }
```

Inserts padding so each field starts on its natural boundary. Total size grows; field access uses standard aligned loads.

Per-view annotation only; field-level alignment overrides are not supported in v1.

### Endianness

**Endianness is required.** Every view declaration must carry `@BigEndian`, `@LittleEndian`, or `@HostEndian`. There is no default — silent host-endian assumptions are too easy a source of bugs when wire formats cross platforms.

```cajeta
@BigEndian       view NetworkFrame { ... }
@LittleEndian    view FileRecord   { ... }
@HostEndian      view CacheEntry   { ... }    // explicit host-order opt-in
```

All multi-byte primitive fields are byte-swapped on access when the view's endianness differs from the host. The compiler emits `bswap` intrinsics only where needed (resolved at compile time per target triple).

---

## Variable-size fields

`String` and `T[]` where `T` is fixed-size are variable-size fields. Their inline layout is **length-prefix + data**:

- `String`: `i32 length` followed by `length` bytes of UTF-8 content.
- `T[]`: `i32 length` followed by `length * sizeof(T)` bytes.

```cajeta
@LittleEndian
view UserRecord {
    int64    id;
    String   username;        // i32 len + UTF-8 bytes
    String   displayName;     // i32 len + UTF-8 bytes
    int32[]  permissions;     // i32 len + (len * 4) bytes
    int64    lastLoginUnix;   // offset is resolved at construction time
}
```

Variable-size fields can appear anywhere in the declaration. Fields **after** a variable-size field have offsets resolved at view-construction time — one length-prefix read per variable-size field, then subsequent fields have constant offsets relative to a cached pointer.

**Construction-time offset cache.** When `UserRecord(buf)` is called, the compiler-synthesized constructor walks the variable-size fields in order, reading each length-prefix and computing the offset of every following field. The resolved offsets are stored in a per-view metadata block on the stack alongside the view value. Per-field access is then a constant-offset GEP against the cached pointer.

**Optimization fast path.** If a view declares all variable-size fields at the end, every fixed field has a compile-time-constant offset and the metadata block can be elided. The compiler detects this layout and emits the fast path automatically — the common case.

**Validation.** Every length-prefix is validated against the remaining buffer size at construction. A length-prefix value larger than the remaining bytes throws `ParseException` from the construction call. See "Security" below.

**Not supported in v1:** arrays of variable-size elements (`String[]`), `T[]` where `T` is itself variable-size. The single-level-deep model covers the protocol patterns we care about; deep variable-size nesting needs offset-tree machinery deferred to v1.1.

---

## Nested views

A view field may itself be of view type. Nested views lay out **inline** — the inner view's bytes appear directly within the outer at its declared offset. There is no pointer indirection.

```cajeta
@BigEndian
view Point {
    int32 x;
    int32 y;
}

@BigEndian
view Line {
    Point start;     // bytes 0..8
    Point end;       // bytes 8..16
}
```

Total size of `Line` is 16 bytes — exactly the sum of its nested layouts.

### Fixed-size inner

If every field of the inner view is fixed-size, the inner contributes a constant block to the outer's layout. All offsets in the outer remain compile-time-constant.

### Variable-size inner

If the inner view contains a `String` / `T[]` / another variable-size view, the inner is itself variable-size. The outer treats it like any other variable-size field: subsequent fields' offsets are resolved at construction. Length-prefix validation recurses — the constructor sweeps every length-prefix at every nesting level.

### Endianness inheritance

A nested view **inherits the outer view's endianness** unless it has its own annotation. The intent of `@BigEndian` on the outer is that every multi-byte primitive in the record is big-endian, regardless of how fields are grouped into nested types.

To opt a sub-region into a different byte order, annotate the nested view's declaration. Mixed-endian records are rare but supported.

### Alignment inheritance

A nested view **inherits the outer view's alignment** unless it has its own `@Align(...)` annotation. Packed outer → packed inner. `@Align(natural)` outer → naturally aligned inner.

### Field access through nesting

`outer.inner.field` is a borrow rooted at `outer`'s buffer per the field-path rule. For fixed-size inners the chained offset is constant; for variable-size inners the cached offset table from construction is used.

### Recursive views forbidden

A view may not contain itself, directly or transitively. The compiler detects this during layout and rejects it.

```cajeta
view Bad {
    Bad child;    // STATIC ERROR — infinite size
}
```

---

## Construction

A view is constructed by calling the view's name as if it were a function, passing the buffer:

```cajeta
byte[] frame = network.read();
RpcHeader h = RpcHeader(frame);          // borrow form — `frame` keeps ownership
```

The form is intentionally constructor-like; under the hood it lowers to compiler-synthesized code that:

1. **Size check** — verifies `buf.length >= MinSize` where `MinSize` is the fixed-prefix size. If not, throws `ParseException`.
2. **Length-prefix validation** — single sweep over the data confirming every variable-size length-prefix stays within `buf.length`. If any overruns, throws.
3. **Offset cache** — for views with variable-size fields, records the resolved offset of each post-variable field in a per-view metadata block.
4. Returns the view value — a small handle carrying `{ buffer-ptr, offset-cache-or-null }`.

After construction, every field access is **bounds-check-free** — the constructor guaranteed every offset lies within the buffer.

### Two construction forms

Cajeta has two construction forms for views, picked by whether the call site uses `#`:

- `RpcHeader(buf)` — **borrow form.** The view borrows the buffer; original owner unchanged. The view's lifetime is statically tied to the buffer's lifetime (see "Borrow semantics integration").
- `RpcHeader(#buf)` — **owning form.** Takes ownership of the buffer; `buf` is moved. The view owns the buffer for the duration of its scope; when the view drops, the buffer drops with it.

```cajeta
// Borrowing — useful when the buffer outlives the view's scope
byte[] frame = bufferPool.acquire();
RpcHeader h = RpcHeader(frame);
process(h);
bufferPool.release(frame);           // frame still owned by us

// Owning — useful for parse-respond flows
byte[] frame = bufferPool.acquire();
RpcHeader h = RpcHeader(#frame);     // frame is moved into h
process(h);
// When h drops at end of scope, the buffer drops too — back into the pool
// via the pool's registered drop hook.
```

The discriminator at the call site is the same `#` token used everywhere else in the memory model. No new syntax.

### Construction failure

Both bounds-check failures and length-prefix-overflow failures throw `ParseException`. The exception carries:

- The offending buffer's actual size.
- The expected minimum size, or the field offset that failed validation.
- The view type name.

Reasoning: server parse loops already have an outer `try/catch` for malformed inputs; exceptions cost nothing on the happy path; null-return forces a check at every call site.

---

## Field access

Read and write IR is the same shape regardless of fixed-vs-variable layout. The only difference is where the offset comes from:

- **Fixed-offset primitive:** `load <type>, ptr (buffer + offset)`. With endian mismatch: `bswap` after the load.
- **Variable-size-following fixed primitive:** the offset is loaded from the per-view metadata block at the cached slot; otherwise identical.
- **Variable-size field (`String`, `T[]`) read:** loads the length-prefix to determine size; returns a typed view over the data region.

### Reads

```cajeta
RpcHeader h = RpcHeader(frame);
int32 m = h.magic;                  // load+bswap from frame+0
int64 id = h.messageId;             // load+bswap from frame+8
```

### Writes

The view is read-write by default. Writing through a view writes through to the buffer:

```cajeta
RpcHeader h = RpcHeader(frame);
h.magic = 0xCAFEBABE;               // bswap+store to frame+0
h.messageId = 42;                   // bswap+store to frame+8
```

The borrow checker enforces that the buffer is exclusively borrowed by the view for the view's lifetime — no other writer can race, no two views can mutate the same bytes.

### Mutation rules

| Operation | Allowed? |
|---|---|
| `h.intField = 42` | Yes |
| `h.flags[i] = X` (writing an element of a fixed-size inline array field) | Yes |
| `h.name.charAt(0) = 'X'` (in-place string byte write) | Yes |
| `h.name = "longer string"` (reassigning a variable-size field) | **Static error** |
| `h.flags = newArray` (reassigning a variable-size field) | **Static error** |

Variable-size field reassignment requires a buffer of a different total size — not supported in-place. The caller must allocate a new buffer and write fields into a fresh view.

---

## Methods on views

A view can declare methods. `this` is the view value (buffer pointer + offset cache); methods can read and write fields and return primitives or owned values.

```cajeta
@BigEndian
view RpcHeader {
    int32 magic;
    int64 messageId;
    int32 payloadLen;

    public boolean isValid() {
        return this.magic == 0xDEADBEEF;
    }

    public int64 totalSize() {
        return 24 + this.payloadLen;     // header bytes + payload
    }

    public void setMagic() {
        this.magic = 0xDEADBEEF;
    }
}
```

### Restrictions

- **No virtual methods.** Views have no vtable.
- **No method-level templates on view methods.** Methods can use the view's own template type parameters (if any), not introduce new ones. (Class-level method-level templates work on `class` declarations per `cajeta-docs/stdlib/MethodLevelTemplate.md`, but views are layout-pinned to a buffer and have no vtable / no per-instance specialization mechanism — adding method-level templates on views would conflate the wire format with the dispatch model.)
- **Methods cannot return borrows into `this`.** A view is already a borrow of its buffer; nested borrow tracking ("this borrow is rooted in the borrow that is `this`") is the kind of complexity deferred to a later spec. If a method needs to expose a sub-region, return the underlying bytes (`byte[]`) or a separately-constructed view value, both of which the caller can use within the view's lifetime via the standard borrow rules.

### What methods *can* do

- Read and write any field.
- Call other methods on `this`.
- Construct nested views over sub-regions of `this`'s buffer (e.g., `this.payloadAsCommand()` constructs and returns a `CommandView(this.payload)`). The returned view borrows the same buffer; its lifetime is bounded by `this`'s lifetime.
- Throw exceptions.
- Return owned values (primitives, freshly-allocated strings, etc.).

---

## Borrow semantics integration

A view is a **borrow of its backing buffer** (in the `RpcHeader(buf)` form) or **owns its buffer** (in the `RpcHeader(#buf)` form). The standard memory-model rules apply.

For borrow-form views (`RpcHeader(buf)`):

- `h` borrows `buf`; `h` cannot outlive `buf` (static error otherwise — `CAJETA_ERROR_VIEW_ESCAPE`).
- `h.field` is a borrow rooted at `buf` (path-based tracking).
- Mutating `buf` directly while `h` is live → static error (alias-mutation).
- Mutating through `h.field = X` invalidates other live borrows of `buf`.
- A view cannot be sent to another fiber (it's a borrow; cross-fiber borrows are forbidden).

For owning-form views (`RpcHeader(#buf)`):

- `h` owns `buf`; `h` drops the buffer when `h` drops.
- `h.field` is a borrow rooted at `h`'s owned buffer.
- The owning view is itself an owner — can be transferred (`#h`), stored in a heap class field, sent to another fiber (subject to the same rules as any other owner).

Full details in `MemoryModel.md`.

---

## Security

Wire-format parsers are a category of code where bugs become CVEs. The view design assumes adversarial input and validates aggressively at construction; per-access reads are free *because* the validation has already happened.

### Threats and mitigations

| Threat | Mitigation |
|---|---|
| Read past buffer end | Size check at construction: `buf.length >= MinSize`. For variable-size views, the length-prefix sweep validates total reachable size. Per-access reads then assume validity. |
| Length-prefix attack (`int32 len = 0xFFFFFFFF`) | Construction-time sweep validates every length-prefix against remaining buffer. Throws on overflow. |
| Buffer mutated while view live | Borrow checker treats the buffer as exclusively borrowed by the view. No other writers; no two views over the same buffer. |
| Two views with conflicting layout over same buffer | Same as above — exclusive borrow precludes this. |
| Endianness confusion | Endianness annotation is **required** on every view declaration. No silent host-endian default. |
| Unaligned access trap (older ARM) | Packed views (`@Align(packed)`, the default) emit unaligned-safe access intrinsics. `@Align(natural)` views emit standard aligned loads. |
| Class-reference pointer reinterpretation | Class refs are forbidden in view fields. There is no construct that lets adversarial bytes become a class pointer through a view. |
| Out-of-bounds via offset arithmetic | All offset computation is compiler-generated from field declarations; no user offset math. |
| Integer overflow in size computation | Construction-time size math uses 64-bit accumulation; overflow throws (rather than wrapping). |
| Use-after-free of underlying buffer | Borrow checker rejects views that outlive their buffer at compile time. |

### What the constructor validates

```cajeta
byte[] frame = network.read();
RpcHeader h = RpcHeader(frame);
```

At construction:

1. `frame.length >= RpcHeader.MinSize` — throws if not.
2. For each variable-size field in declaration order: read the length-prefix, verify `(currentOffset + prefix + length) <= frame.length`, throws if not. Recurses into nested variable-size views.
3. Records the resolved offset of every post-variable-size field in the view's offset cache.
4. Returns the view handle. Total work: one bounds check + one read per variable-size length-prefix. For fully-fixed views, just the bounds check.

After step 4, every field access is unchecked — the constructor's guarantees are load-bearing.

### What the constructor does *not* check

- **Field value semantics.** The constructor doesn't know that `h.magic` is supposed to be `0xDEADBEEF`. That's the caller's responsibility (`if (!h.isValid()) throw ...`).
- **Cross-field invariants.** The constructor doesn't know that `h.payloadLen` should match `frame.length - 24`. The caller validates protocol-specific invariants.
- **String encoding validity.** A `String` field's bytes are not verified to be valid UTF-8 at construction. Operations that decode the string (iteration, code-point access) will throw on invalid sequences.

The principle: the constructor guarantees **no field access reads past the buffer end**. Everything else is application logic.

---

## Wire-format versioning

The view declaration **is** the wire spec. Adding or reordering fields, changing widths, endianness, or alignment breaks compatibility with existing producers and consumers — silently in the field-rename case, with construction-time errors in the size case.

Recommended versioning patterns:

- **Embed a version field** at a known offset (typically the first field) and dispatch to different view types per version.
- **Use distinct view types per protocol version** (`RequestHeaderV1`, `RequestHeaderV2`), with conversion routines between them.

No automatic backwards compatibility — that's a protocol design problem, not a language problem.

```cajeta
@BigEndian
view ProtoVersion {
    int32 version;
}

byte[] frame = network.read();
ProtoVersion v = ProtoVersion(frame);

match v.version {
    1 => handleV1(RequestHeaderV1(frame)),
    2 => handleV2(RequestHeaderV2(frame)),
    _ => throw new UnsupportedProtocolException(v.version),
}
```

---

## Cross-platform considerations

### Unaligned access

x86 handles unaligned loads/stores transparently with negligible cost. ARMv7+ allows them at a small perf cost; pre-ARMv6 traps. The compiler emits unaligned-access intrinsics by default for portability. If a view uses `@Align(natural)`, accesses are aligned and use standard load/store.

### Byte order

Network protocols are typically big-endian. Use `@BigEndian` on the wire view. Most file formats are little-endian; use `@LittleEndian`. The annotation is required precisely so that the choice is explicit — silent host-endian assumptions break when code moves between architectures.

### Wide primitives

`int128` / `uint128` / `float128` may not have native hardware support on all targets. The compiler emits software fallbacks where needed.

---

## What this is not

- **Not for self-describing formats.** Wire views cover fixed-layout protocols. Ion, JSON, MessagePack, CBOR, BSON, Protobuf, Avro need streaming parsers — see `cajeta.codec.*` libraries.
- **Not a replacement for `class`.** Use views only for layout-stable POD overlay; `class` for everything with behavior, identity, or class-reference fields.
- **Not a replacement for `struct`.** Structs are stack-allocated value aggregates with full field-type freedom (including class refs). Views are buffer overlays with the byte-encodability restriction.
- **Not safe across versions.** Field changes are silently breaking. Add explicit versioning.
- **Not suitable for shared mutable state across threads.** Multi-threading isn't in v1; when added, shared-buffer access will need synchronization primitives.

---

## Errors caught statically

### Why views cannot be class fields

A view is a borrow (or owner) of its buffer. A class field can be stored anywhere — heap, container, sent across method boundaries via the field's enclosing instance — and may outlive the buffer the view was constructed over. The borrow check would have to track every field's buffer-of-origin and prove the field can never be observed after its buffer drops; the analysis is intractable in the general case.

Template instantiations are caught by the same rule. `HashMap<int32, MyView>` instantiates `V[] vals` as `MyView[]` — an array-of-view class field, rejected at instantiation time. `Optional<MyView>` has a `T value` field, also rejected. Users who want a map keyed or valued by view-shape data store the underlying `byte[]` in the class and reconstruct the view per access; the per-construction cost is one bounds check + one length-prefix sweep, both of which fall out of register pressure on the read path.

Nested views (`view A { view B inner; }`) are NOT rejected — the inner lays out inline within the outer's buffer per the doctrine in § Nested views.

| Error | Caught by |
|---|---|
| View outliving its buffer (borrow form) | Memory model scope check (`CAJETA_ERROR_VIEW_ESCAPE`) |
| Mutating the backing buffer while view is live | Memory model alias-mutation rule |
| Two views constructed over the same buffer with both live | Exclusive-borrow check |
| Reassigning a variable-size field through a view | Wire-format mutation rule |
| Class-reference field in a view declaration | Layout pass — forbidden field type |
| Recursive view definition (direct or transitive) | Layout-cycle detection during type registration |
| View declaration without `@BigEndian` / `@LittleEndian` / `@HostEndian` | Declaration parse — endianness annotation required |
| Method returning a borrow into `this` (view-internal nested borrow) | Method codegen — v1 limitation |
| Method declared as virtual | Declaration parse — views have no vtable |
| Method declared with its own type parameters | Declaration parse — v1 limitation |
| View used as a class field — directly, via array element, or via a template-T instantiation (`HashMap<view, X>`, `ArrayList<view>`, `Optional<view>`, etc.) | `CajetaClass::generatePrototype` field-type check (`CAJETA_ERROR_VIEW_AS_CLASS_FIELD`) |
| View used as an interface implementation | Type-system check — views are not assignable to interface slots |

## Errors caught at construction (runtime)

| Error | Thrown |
|---|---|
| Buffer shorter than view's fixed-prefix minimum size | `ParseException` |
| Length-prefix value overruns the buffer | `ParseException` |
| Length-prefix in a nested view overruns | `ParseException` |
| Construction-time size arithmetic overflow | `ParseException` |

---

## Implementation outline

For the language implementer:

1. **Parser:** recognize `view` keyword (new lexer token `VIEW`); parse `viewDeclaration` with field list and optional method list. Accept `@BigEndian`, `@LittleEndian`, `@HostEndian`, `@Align(...)` annotations; require exactly one endianness annotation per view.
2. **Type system:** add `CajetaView` (sibling of `CajetaClass` and `CajetaStruct`) with explicit layout computation. Compute fixed-prefix size; identify variable-size fields; pre-compute the constant-offset fast path when applicable. Recurse into nested view fields, inheriting endianness/alignment from the outer unless overridden. Detect layout cycles and reject. Reject class-reference field types.
3. **Constructor synthesis:** for each view type, emit two construction functions — `View(byte[])` returning a borrow view, and `View(#byte[])` returning an owning view. Both emit the size check + length-prefix-validation sweep + offset-cache build. Resolve the bare-call construction syntax at the parser/AST level so `RpcHeader(buf)` lowers to the synthesized constructor.
4. **Field accessor codegen:** for each field, emit a read and write accessor that performs the offset computation, byte-swap if needed, and the load/store. For variable-size-following fields, the offset comes from the runtime offset cache; for fixed fields, it's compile-time-constant.
5. **Variable-size offset cache:** at construction time, walk variable-size fields and cache resolved offsets in the view's metadata block. Recurse into nested variable-size views so every length-prefix at every nesting level is validated and offsets are resolved before construction returns.
6. **Borrow checker integration:** treat borrow-form construction as a borrow of the byte array; treat owning-form construction as a move of the array into the view. Treat each field access as a path-based borrow. Reject views whose backing buffer goes out of scope before the view does. Reject sending a borrow-form view to another fiber.
7. **Mutation rule enforcement:** at the AST level, reject reassignment of variable-size view fields.
8. **Endianness intrinsics:** emit `bswap` only when view endianness differs from host endianness (resolved at compile time per target triple).
9. **Calling convention:** view values pass by pointer at call sites and return sites (cajeta's existing aggregate pass-by-pointer rule). The view value is a small handle (buffer pointer + optional offset-cache pointer); passing it by pointer is cheaper than by-value when methods are large.
10. **Owning-view drop:** when an owning-form view drops, drop the contained buffer (single LLVM `free` call against the buffer's stored ownership).

---

## Examples

### Simple fixed-size record

```cajeta
@HostEndian
view PixelRGBA {
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

```cajeta
@BigEndian
view RpcHeader {
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

```cajeta
@LittleEndian
view UserRecord {
    int64    id;
    String   username;          // variable
    String   displayName;       // variable
    int32[]  permissions;       // variable
    int64    lastLoginUnix;     // offset resolved at construction
}

byte[] recordBytes = db.read(key);
UserRecord u = UserRecord(recordBytes);
println(u.username + " has " + u.permissions.length() + " permissions");
println("last login: " + u.lastLoginUnix);
```

The compiler resolves offsets for `displayName`, `permissions`, and `lastLoginUnix` at construction by reading the preceding length-prefixes once.

### Nested views

```cajeta
@BigEndian
view Point {
    int32 x;
    int32 y;
}

@BigEndian
view BoundingBox {
    Point topLeft;        // bytes 0..8   (inherits @BigEndian)
    Point bottomRight;    // bytes 8..16
    int32 zIndex;         // bytes 16..20
}

byte[] frame = readBox();
BoundingBox box = BoundingBox(frame);
println(box.topLeft.x + ", " + box.bottomRight.y);
```

### Writing a response

```cajeta
@BigEndian
view ResponseHeader {
    int32 magic;
    int16 version;
    int16 status;
    int64 messageId;
    int32 payloadLen;
}

// Acquire a buffer from the pool, build the response in place.
byte[] outBuf = bufferPool.acquire();
ResponseHeader resp = ResponseHeader(outBuf);
resp.magic = 0xDEADBEEF;
resp.version = 1;
resp.status = 200;
resp.messageId = request.messageId;
resp.payloadLen = 0;
network.send(outBuf, resp.totalSize());
bufferPool.release(outBuf);
```

### Owning view across a parse-respond cycle

```cajeta
async void handleRequest() {
    byte[] frame = await connection.readFrame();
    RpcHeader h = RpcHeader(#frame);    // view takes ownership of the buffer

    // h is now an owner; can be transferred to another fiber, stored, etc.
    await processInline(#h);

    // When h finally drops, the buffer drops with it (back to the pool
    // via the pool's registered drop hook).
}
```
