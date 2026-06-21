# Spec: StringBuilder small-string optimization (SSO) + fixed-size inline array fields

## 1. Definition

### 1.1 Purpose
Let a `StringBuilder` accumulate short strings with **zero heap allocation during the
build** by storing the first bytes in an **inline buffer embedded in the object itself**.
A build that fits the inline buffer — and is declared `stack StringBuilder()` — lives
entirely on the stack until `toString()` materializes the result; a build that overflows
it transparently spills to a doubling heap buffer with byte-identical output.

### 1.2 Problem
Today every `StringBuilder` heap-allocates its backing buffer immediately and again on
each growth. For the common case of building small strings (names, keys, numbers, short
messages), that is one or more heap allocations per build that the competitor languages
avoid (C++ `std::string` SSO, Rust `SmallVec`/`SmallString`, Go stack-escape analysis).
Cajeta has no inline-buffer mechanism: the natural primitive — a fixed-size inline array
**field** (`int8[64]`) — is only half-implemented (works as a local; as a class field the
type system does not carry the length, so it is laid out as a pointer and crashes on
access). SSO is therefore blocked on completing that compiler feature.

### 1.3 Scope
Two layers, built together (foundation first):
- **(A) Fixed-size inline array fields** — a general compiler capability: a class field
  `T[N]` is stored inline (N elements, no pointer, no heap), for any element type `T`.
- **(B) StringBuilder SSO** — a consumer of (A): an inline byte buffer + a heap spill
  buffer as **separate fields**, with a doubling spill path.

### 1.4 Constraints
- **Separate-fields design** (not a space-optimal union): the inline buffer and the heap
  spill pointer coexist as distinct fields. Cajeta has no unions; unioning them is a
  future optimization, out of scope here.
- The inline capacity is a fixed compile-time constant: **64 bytes** for `StringBuilder`.
- Reuse the established hot/cold idiom (`@Inline` hot path + `@NoInline` cold grow).

### 1.5 Non-goals
- 1.5.1 Union/overlapping SSO layout (needs union support — deferred).
- 1.5.2 Eliminating `toString()`'s result allocation — the result `String` escapes the
  builder, so it is inherently heap; SSO removes **build-time** heap, not the result.
- 1.5.3 Resizable / runtime-sized inline buffers (the inline length is a compile-time
  constant by definition).
- 1.5.4 A general union/variant type system feature.

## 2. Feature: Fixed-size inline array fields

A class field declared `T[N]` (N a compile-time integer literal) is stored as N
contiguous elements **inline** in the enclosing object, distinct from a heap array
reference `T[]`. The capability is general over the element type `T` (primitives,
structs, etc.).

### 2.1 Use cases
- 2.1.1 As a class author, when I declare `int8[64] buf;` as a field, then the object's
  layout reserves 64 inline bytes at that field's offset (no pointer slot, no heap body).
- 2.1.2 As a class author, when I read `obj.buf[i]` (runtime `i`), then it reads the
  i-th inline element directly (address = field offset + i·sizeof(T)), with no pointer
  load and no array-header indirection.
- 2.1.3 As a class author, when I write `obj.buf[i] = v`, then it stores into the i-th
  inline element directly (this is the path that currently SIGSEGVs).
- 2.1.4 As a runtime, when an object holding a `T[N]` field is dropped, then the inline
  field is **not** freed (it is inline storage, not a heap allocation) — no double/invalid
  free.
- 2.1.5 As a class author, `T[N]` (fixed, inline) and `T[]` (heap reference) are
  **distinct types**: `int8[64] a;` and `int8[] b;` lay out and behave differently, and
  the type system tells them apart.
- 2.1.6 As a class author, the feature is general over `T`: `int32[4] rgba;`,
  `float64[3] xyz;`, etc., each lay out as N inline elements of the element's size.
- 2.1.7 As a class author, an out-of-range constant index on a `T[N]` field is a
  compile-time error where statically determinable; runtime bounds behavior matches the
  language's existing array-bounds policy (`--bounds`).
- 2.1.8 As an existing program, declaring no inline array fields, behavior and layout of
  every existing class are **unchanged** (heap `T[]` fields keep their pointer layout).

## 3. Feature: StringBuilder small-string optimization

`StringBuilder` gains an inline byte buffer; appends stay inline until they exceed it,
then spill once to a doubling heap buffer. Output is identical to a non-SSO build.

### 3.1 Use cases
- 3.1.1 As a developer, when I append a total of ≤ 64 bytes to a `StringBuilder`, then
  **no heap allocation occurs during the build** (the bytes live in the inline buffer);
  only `toString()` allocates (the escaping result).
- 3.1.2 As a developer, when I write `stack StringBuilder()` and build ≤ 64 bytes, then
  the builder object **and** its bytes are entirely stack-resident until `toString()`.
- 3.1.3 As a developer, when my appends exceed 64 bytes, then the builder transparently
  spills to a heap buffer (copying the inline bytes over once) and continues; subsequent
  growth doubles the heap buffer as today.
- 3.1.4 As a developer, `toString()` returns a correct owned `#String` whether the build
  stayed inline or spilled.
- 3.1.5 As a developer, the produced string is **bit-for-bit identical** to the same
  sequence of appends on a non-SSO builder, across the inline→spill boundary (e.g. a
  build of exactly 64, 65, 0, and large sizes).
- 3.1.6 As a developer, `count()` / `isEmpty()` / `appendBytes()` behave identically in
  inline and spilled modes.
- 3.1.7 As a maintainer, `String.replace` and any other current `StringBuilder` consumers
  continue to pass unchanged (the public surface is preserved; only internals change).

### 3.2 Correctness invariants
- 3.2.1 At all times exactly one of {inline buffer, heap buffer} holds the live bytes,
  selected by the `spilled` flag; `len` is the byte count in whichever is live.
- 3.2.2 Spilling is one-way (inline → heap) and copies all `len` live bytes before any
  further append.
- 3.2.3 `cap` equals the inline capacity while inline, and the heap buffer's size while
  spilled; an append never writes past `cap` without growing.

## 4. Acceptance themes (cross-cutting)
- 4.1 Correctness: inline-array-field read/write/drop is sound (no crash, no leak, no
  double free); SSO output matches non-SSO output across the boundary.
- 4.2 Non-regression: the full existing test + benchmark suite is unaffected; no change
  to programs without inline array fields.
- 4.3 Observable win: a ≤64-byte `stack StringBuilder()` build performs **zero** heap
  allocations during the build — verified by the **absence of any heap-allocation call**
  (`malloc` / `__cajeta_new_array*`) in the inlined small-build code path (disasm/IR
  inspection), not merely by timing. (No new allocation-counting test infrastructure is
  introduced; the symbol-absence check is the gate.)
