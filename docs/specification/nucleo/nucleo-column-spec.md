# Núcleo Column — Specification

> Status: draft for review (2026-06-23). The **Arrow-laid-out columnar buffer** —
> núcleo's physical substrate (`dev.cajeta.nucleo.column`). Layer-1b foundation.
> Companion analysis: `python-stack-analysis.md` §2.3/§2.4; design context:
> `target-experience.md` §1/§7, `language-foundations.md` §2.5. Schema companion:
> `records-spec.md` (a record describes a schema; this spec defines the per-field
> column storage that schema transposes onto).
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions
> (the *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §10,
> to be resolved when this spec is turned into a plan. Outline-numbered for
> addressability.

## 1. Definition

### 1.1 Purpose
A **column** is a typed, contiguous, Arrow-laid-out buffer of values — the physical
substrate every higher núcleo surface stands on. A dataframe column, a tensor's
backing, a geometry-attribute channel, and a Gaussian-splat field are all *the same
typed columnar buffer*. `Column<T>` exists to give núcleo **one** physical
representation that is, by construction, byte-compatible with both the stdlib tensor
substrate and the wider Arrow ecosystem — so the fused/differentiable engine and the
zero-copy interop seam fall out without a conversion layer.

The load-bearing invariant this spec builds around: **a non-null numeric `Column<T>`
is bit-identical to a tensor buffer** — the same bytes, no marshalling (analysis
§2.3).

### 1.2 Scope
- The physical layout: contiguous typed value buffer; a **separable** validity bitmap;
  offsets+data buffers for variable-length types; 64-byte padding/alignment.
- Nullability modelled as a **type distinction**: `Column<T>` (non-null, == tensor
  bytes) vs. `Column<T?>` (nullable, carries a validity bitmap).
- The **C Data Interface** seam — `ArrowSchema`/`ArrowArray` (a ~2-struct frozen C
  ABI) — for zero-copy in-process export/import, implemented by **matching the
  structs** (no `libarrow`).
- **MX formats** (MXFP4 etc.) carried as Arrow **extension types** (logical type over
  physical storage).
- A zero-copy **view** of a non-null numeric column as a `cajeta.math.Tensor` (and the
  inverse).
- The **Tensor Arrow retrofit** in stdlib `cajeta.math` (additive): 64-byte aligned
  allocation + a C-Data-Interface export/import seam alongside the existing
  `TensorProtocol`.

### 1.3 Non-goals
- **Compute kernels.** Arrow's compute layer is **not** adopted — element-wise,
  reduction, contraction, and differentiable kernels are núcleo's own (they live in
  `nucleo.expr`/`nucleo.autograd`, operating directly on these buffers).
- **An Arrow logical type hierarchy.** núcleo owns its own type model; this spec
  adopts only Arrow's *physical layout* and the C ABI structs, not Arrow's type tree.
- **A `libarrow` dependency.** Interop is by matching the frozen C structs only. A
  young language must not be an island, but must also not take a heavy C++ dependency
  to avoid being one — the C Data Interface is exactly that escape (analysis §2.4).
- **The expression/fusion engine, the dataframe, indexing, autograd** — separate specs
  (`nucleo-expr`, `nucleo-frame`, `nucleo-index`, `nucleo-autograd`). This spec is the
  buffer only.
- **Nested/struct/list-of-list column types beyond a single offsets level** — v1 covers
  fixed-width primitive and single-level variable-length (string/binary) layouts;
  deeper nesting is deferred.
- **A device-native columnar format.** The **host** side is the contract; the device
  representation is núcleo's own (see §8).

### 1.4 Relationship to existing constructs
- **stdlib `cajeta.math.Tensor` is the substrate.** Its `Storage<T>` is already a
  single contiguous, C-order, **dense/null-free** buffer — i.e. already the non-null
  `Column<T>` case of §1.1. The retrofit (§7) makes that byte-compatibility explicit
  and adds the interop seam; it does **not** rebuild the tensor.
- **`TensorProtocol`** is the existing DLPack-style strided interop descriptor on
  `Tensor` (`{ Storage borrow, offset, shape, strides, DType, device, read-only }`).
  The C-Data-Interface seam is **additive alongside** it — `TensorProtocol` stays the
  in-language zero-copy round-trip; the Arrow seam is the cross-ecosystem one.
- **`ColumnVector<T>`** was *specified yet never built* — the codec readers decode
  straight to raw arrays. So the column type is **greenfield**, designed
  Arrow-conformant from day one (no legacy layout to honor).
- **Records** (`records-spec.md`) describe a schema; `Table<Tick>` transposes it to one
  `Column<T>` per field (SoA). A record is the *type-level* descriptor; this spec is the
  *physical* per-field buffer it lands on.
- **Templates are monomorphized** — `Column<T>` is real per-`T` packed storage (e.g.
  `Column<MXFP4>` is genuinely packed), not an erased generic.

## 2. Physical layout — contiguous typed buffers

A column's bytes follow the Arrow in-memory columnar layout: a fixed-width primitive
column is one contiguous value buffer; a variable-length column is an offsets buffer
plus a data buffer; an optional validity bitmap rides alongside (§3). All buffers are
64-byte padded/aligned (§6).

**Use cases**
- **2.1** As a núcleo author, when I build a fixed-width numeric `Column<float32>`, then
  its values occupy a single contiguous, C-order value buffer — the same byte order a
  1-D tensor of the same dtype would have.
- **2.2** As a núcleo author, when I build a variable-length `Column<String>`, then it is
  stored as an offsets buffer (one entry per element plus a final length) over a single
  contiguous data buffer, matching Arrow's variable-length layout.
- **2.3** As a núcleo author, when I read element `i` of a fixed-width column, then the
  access is a direct indexed load into the value buffer (no per-element indirection),
  preserving the dense-buffer performance the tensor substrate already has.
- **2.4** As a núcleo author, when a column's dtype is a sub-byte packed type (e.g.
  MXFP4), then the value buffer is the genuinely packed representation (monomorphized
  storage), and its logical type rides as an extension type (§5).

> **TBD (plan-time):** Whether v1 ships only fixed-width + single-level variable-length,
> or also large-offset (64-bit offsets) variants; and the exact set of primitive dtypes
> a column admits (expected: the `cajeta.math` `DType` set).

## 3. Nullability as a type distinction — `Column<T>` vs. `Column<T?>`

Arrow's null representation is **separable**: a validity bitmap is omitted entirely when
the null count is zero. núcleo lifts that separability into the **type**: a non-null
column and a nullable column are *different types* with *different physical footprints*.

**Use cases**
- **3.1** As a núcleo author, when I build a non-null `Column<float32>`, then it has **no
  validity bitmap** and its value buffer is **bit-identical** to the equivalent tensor
  buffer — this is the load-bearing invariant (§1.1, analysis §2.3).
- **3.2** As a núcleo author, when I build a nullable `Column<float32?>`, then it carries
  a separate validity bitmap (1 bit per element, set = valid) alongside the value buffer;
  the value buffer itself is unchanged in layout.
- **3.3** As a developer, when I read a value from a `Column<T>` (non-null), then there is
  no validity check — the type guarantees presence — and no nullability branch is emitted.
- **3.4** As a developer, when I read element `i` of a `Column<T?>`, then validity is
  consulted (a missing element is a real absence — not NaN-as-missing, which the dataframe
  design explicitly rejects).
- **3.5** As a núcleo author, when an imported external Arrow array has a null count of
  zero, then it may be admitted as a `Column<T>` (no bitmap), and when its null count is
  nonzero it admits only as `Column<T?>` — the type reflects the physical reality.

> **TBD (plan-time):** Whether a `Column<T?>` whose runtime null-count is zero may be
> *narrowed* to a `Column<T>` view in-place (bitmap present but all-set) — a zero-copy
> downgrade — or whether the type is fixed at construction.

## 4. The C Data Interface — zero-copy in-process interchange

Interop is via the Arrow **C Data Interface**: two frozen C structs, `ArrowSchema` (the
type/format descriptor) and `ArrowArray` (the buffer pointers + lengths + a release
callback). núcleo implements these by **matching the struct layout** — no `libarrow`
link, no Arrow runtime. Export hands out the structs over the column's live buffers;
import wraps externally-owned structs and honors their release callback.

**Use cases**
- **4.1** As a developer, when I call `column.exportArrow()`, then I receive
  `ArrowSchema`/`ArrowArray` handles describing the column's live buffers — pyarrow,
  Polars, or DuckDB read it **with no serialization and no copy**.
- **4.2** As a developer, when an external library hands me an `ArrowArray`/`ArrowSchema`
  pair (e.g. a numpy/pyarrow array across the same ABI), then I import it as a
  `Column<T>` / `Column<T?>` **zero-copy** — the buffers are borrowed, not copied.
- **4.3** As a developer, when I import an external Arrow array, then núcleo owns the
  responsibility of invoking the producer's **release callback** exactly once when the
  imported column's borrow ends — the cross-ABI ownership contract is honored (no leak,
  no double-free).
- **4.4** As a developer, when an exported column is consumed by the foreign side, then
  the foreign side invokes **our** release callback when done, and our buffers stay alive
  for the duration of the borrow — the export descriptor is a borrow, like
  `TensorProtocol` (round-trip promptly; outliving the source storage is unsafe).
- **4.5** As a núcleo author, when I export/import a column whose format string encodes a
  type núcleo does not model, then the bytes still move and the format is preserved
  (graceful degradation — see §5), rather than failing the interchange.

> **TBD (plan-time):** [C-ABI] Exact mapping of the release-callback ownership across the
> language's deterministic-memory model (who registers the drop, how the borrow is held —
> likely mirroring `TensorProtocol`'s `Object`-erased backing to keep teardown sound);
> and whether the import airlock returns `Column<?>` requiring an `instanceof`-guarded
> reified capture (as `Tensor.fromProtocol` does today).

## 5. MX formats as Arrow extension types

MX micro-scaling formats (MXFP4 etc.) ride as Arrow **extension types**: a logical type
layered over a physical storage type. The extension name + metadata travel in the schema;
the physical bytes travel in the array. This means the bytes move across interop **even
where the receiving tool does not know the semantics**.

**Use cases**
- **5.1** As a developer, when I view a column `as<MXFP4>()`, then its logical type is the
  MX extension while its physical storage is the underlying packed buffer — a logical view
  over physical bytes, not a re-encode.
- **5.2** As a developer, when I export an MXFP4 column over the C Data Interface, then the
  extension type name + metadata are emitted in the `ArrowSchema`, and a tool that knows
  MXFP4 reconstructs the logical type while a tool that doesn't **still moves the physical
  bytes** (graceful degradation — analysis §2.4).
- **5.3** As a developer, when I import an Arrow array tagged with an extension type núcleo
  recognizes, then it is admitted as the corresponding logical `Column<...>`; when the
  extension is unknown, the underlying physical column is still importable.

> **TBD (plan-time):** Which MX/sub-byte formats are extension types in v1, and the exact
> extension-name registry (matching the ecosystem's emerging MX Arrow conventions where
> they exist).

## 6. Alignment and padding — 64-byte

Arrow's layout convention is 64-byte padding/alignment of buffers (cache-line and
SIMD-friendly; required for some consumers). núcleo adopts it.

**Use cases**
- **6.1** As a núcleo author, when I allocate a column's value buffer, then it is aligned
  to a 64-byte boundary and its allocation is padded up to a 64-byte multiple, so SIMD
  kernels and Arrow consumers can read it without an alignment fault or a fix-up copy.
- **6.2** As a núcleo author, when I export a column over the C Data Interface, then the
  buffers I hand out already satisfy the alignment a strict Arrow consumer expects — the
  export is zero-copy because the bytes were laid out conformantly from allocation.

> **TBD (plan-time):** Alignment policy — **always** 64-byte align every buffer, vs. a
> **threshold** (align only buffers above some size; small columns use natural alignment).
> Affects allocator design and the tensor retrofit (§7) symmetrically.

## 7. The Tensor Arrow retrofit (additive, in `cajeta.math`)

`cajeta.math.Tensor` is already contiguous and dense — i.e. already the non-null column
case. The retrofit makes the byte-compatibility a guarantee and adds the cross-ecosystem
seam, **additively**: no behavior of `Tensor`/`Storage`/`TensorProtocol` changes.

**Use cases**
- **7.1** As a stdlib author, when `Tensor`/`Storage` allocates its backing, then it does
  so 64-byte aligned (per the §6 policy), so a contiguous non-null numeric tensor is
  byte-compatible with a `Column<T>` of the same dtype with **no fix-up copy**.
- **7.2** As a developer, when I call `tensor.exportArrow()`, then I get
  `ArrowSchema`/`ArrowArray` handles over the tensor's live buffer — the same C-Data seam
  as a column — **alongside** the existing `tensor.protocol()` DLPack-style descriptor
  (both remain available; neither replaces the other).
- **7.3** As a developer, when I call `Tensor.importArrow<float32>(handle)`, then an
  external Arrow array is imported zero-copy as a `Tensor`, honoring the producer's release
  callback — mirroring `Tensor.fromProtocol` but across the Arrow ABI.
- **7.4** As a developer, when I view a non-null numeric `Column<float32>` as a tensor
  (`column.asTensor()`), then it is a **zero-copy** view — the validity bitmap is absent,
  so the column's value buffer *is* the tensor's buffer (no allocation, no copy); the
  inverse (`Column.fromTensor`) is likewise zero-copy.
- **7.5** As a developer, when I attempt to view a **nullable** `Column<T?>` as a tensor,
  then it is **not** a zero-copy view (the tensor substrate is dense/null-free) — the
  operation either requires an explicit fill/drop-nulls (materializing a dense buffer) or
  is a compile error on the nullable type, never a silent reinterpretation.

> **TBD (plan-time):** Whether `asTensor()` on `Column<T?>` is a compile error (type-level
> refusal) or a runtime operation requiring an explicit null-handling argument; and whether
> the retrofit reuses `TensorProtocol`'s `Object`-erased borrow machinery for the Arrow
> seam's release-callback safety.

## 8. Device caveat — host is the contract, device is ours

Arrow is host-memory-centric. The host-side C Data Interface is the interop **contract**;
the **device** representation of a column is núcleo's own. `ArrowDeviceArray` is treated as
a **bridge** when a device handoff is needed, **not** a foundation — Arrow's CPU-shaped
assumptions must not leak into device buffer design (analysis §2.4 device caveat).

**Use cases**
- **8.1** As a núcleo author, when I export a host-resident column over the C Data
  Interface, then the host contract (§4) applies unchanged.
- **8.2** As a núcleo author, when a column is device-resident, then its physical
  representation is núcleo's own device buffer design (aligned with `cajeta.gpu`), **not**
  dictated by Arrow's host layout; an `ArrowDeviceArray`-style bridge is used only at an
  explicit device-handoff boundary.

> **TBD (plan-time):** Device-side representation details — the device buffer layout, the
> column↔device-buffer transition (mechanically, SoA host columns map onto planar device
> buffers, per analysis §4.6), and if/how `ArrowDeviceArray` is matched for a device bridge.

## 9. Acceptance criteria (spec-level)
- A non-null `Column<T>` for a numeric `T` is **bit-identical** to the equivalent
  `cajeta.math.Tensor` value buffer (validity bitmap omitted when null-count is zero).
- A nullable `Column<T?>` carries a separable 1-bit-per-element validity bitmap; the value
  buffer layout is unchanged from the non-null case.
- A column exports to and imports from `ArrowSchema`/`ArrowArray` **zero-copy**, by matching
  the C structs, with **no `libarrow` link** and honoring the cross-ABI release callback.
- pyarrow/Polars/DuckDB can read an exported column live; an externally produced Arrow array
  imports zero-copy.
- A non-null numeric column views as a `Tensor` with no copy; the inverse holds.
- An MXFP4 column carries through interop as an Arrow extension type — bytes move even where
  the semantics are unknown.
- All buffers satisfy the chosen 64-byte alignment policy.
- The Tensor retrofit is **additive**: existing `Tensor`/`Storage`/`TensorProtocol` behavior
  is unchanged.

## 10. Open questions (resolve at plan time)
- **64-byte alignment policy** — always vs. threshold (§6); applies symmetrically to the
  column allocator and the tensor retrofit (§7).
- **Device-side representation details** — device buffer layout, the host-column↔device-buffer
  transition, and whether `ArrowDeviceArray` is matched as a bridge (§8).
- **Codec readers materializing into `Column`** — the codec lib's Parquet/ORC/CSV/JSON readers
  currently decode to raw arrays (`ColumnVector` was never built); whether/how they are
  retargeted to materialize directly into Arrow-conformant `Column<T>` (zero-copy where the
  on-disk encoding already matches the in-memory layout). *(Cross-spec with the codec lib.)*
- **[C-ABI]** Release-callback ownership across the deterministic-memory model and the import
  airlock's return type (`Column<?>` + reified capture vs. typed) (§4).
- **Nullable→non-null narrowing** — whether an all-valid `Column<T?>` can be viewed as
  `Column<T>` in place, and whether `asTensor()` on `Column<T?>` is a compile error or a
  runtime null-handling operation (§3, §7).
- **v1 layout coverage** — fixed-width + single-level variable-length only, or also large
  (64-bit) offsets; the admitted primitive dtype set (§2).
- **MX extension registry** — which MX/sub-byte formats are extension types in v1 and their
  extension-name conventions (§5).
