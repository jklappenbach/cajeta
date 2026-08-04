# debug-type-sidecar — spec

## 1. Definition
Variable inspection decodes a stopped value by resolving its canonical type
through `CajetaType::of` — the compiler's live type world. That world exists only
after a compile. On a **whole-program cache HIT** the debug launch loads cached
bitcode and never constructs a `Compiler` (`CajetaJitHost.cpp:891`; the code notes
"cache HIT (no type world)" at line 147), so `CajetaType::of` resolves nothing and
`ValueInspector` renders every non-primitive as `<unknown>` with no children.

A cache hit is the common launch (same binary, unedited source), so today
inspection works only on the first launch after a recompile. This regression is
invisible to `cajeta_debug_test`, which uses fresh temp dirs — every test is a
cache miss.

The fix: carry the type-layout facts `ValueInspector` needs in a **sidecar** of
the whole-program slot, exactly as the `dbgloc` table and `EntryArgsABI` already
are (both exist precisely because a hit has no type world). A global
`DebugTypeTable` is populated from the type world on a cold build (and written to
the sidecar) and loaded from the sidecar on a hit; `ValueInspector` reads only the
table, so cold and warm decode identically.

### 1.1 Scope
- A serializable per-type layout record sufficient for leaf/array/object/
  collection decode, keyed by canonical type name.
- Emit it during cold codegen (type world live) into a slot sidecar.
- Load it on a cache hit into the same global table.
- Repoint `ValueInspector` at the table, not `CajetaType`.

### 1.2 Non-goals
- No change to breakpoints, stepping, frames, or the `dbgloc`/`EntryArgsABI`
  sidecars (they already flow cold→hit correctly).
- No new decode capability — this reproduces cold-path behavior on the warm path,
  nothing more. New decode features land in `debugger-variable-inspection`.
- Not a general reflection/RTTI facility; the table serves debug decode only.

### 1.3 Fixed assumptions
- The JIT host target DataLayout is fixed across a cold build and a later warm
  load of the same slot, so **byte offsets and strides computed cold are valid on
  the hit** — the table stores resolved offsets, not `(struct, index)` pairs, so
  the consumer needs neither `CajetaType` nor a live `StructType` lookup warm.
- On a hit, `DbgVar` still carries each local's canonical type-name string (it is
  emitted into the code by `__cajeta_dbg_local`; native values already render
  warm), so a type name is always available as the table key.

---

## 2. The type-layout table
### 2.1 Requirements
- **2.1.1** One record per type that can appear as a debug value — a local's
  type, transitively its fields' types, array element types, and collection
  element/key/value types (the reachable closure from the program's debug
  locals). A record holds only what a decoder reads.
- **2.1.2** A record carries: canonical name; a kind (`Leaf` | `Array` | `Object`
  | `Collection`); `isValueType` (inline vs pointer at the top level); for an
  object, ordered fields `{declaredName, typeName, byteOffset, storage}`; for an
  array, `{elementTypeName, elementStride, elementStorage}`; for a collection,
  the collection kind (`ArrayList` | `HashMap`) plus its object fields (so the
  logical decoder finds `data`/`sizeCount`/`slots`/`ctrl` by name at their
  offsets). `storage` is `Inline` | `Pointer`, the same distinction
  `ValueInspector` already draws.
- **2.1.3** Populated on a cold build from the live type world, using the exact
  logic `ValueInspector` uses today (`getFieldLlvmIndex` +
  `getStructLayout->getElementOffset`, `CajetaArray::elementStrideBytes`, the
  `getElementLlvmType` storage rule), so an offset in the table equals the offset
  the JIT'd code stores at.
- **2.1.4** String keeps a `Leaf` record; its decode ABI is the existing
  `EntryArgsABI`/String-ABI facts, unchanged.
- **2.1.5** A global table, populated in place like `globalDbgLocTable()` — one
  authoritative instance the DAP server reads.

### 2.2 Use cases
- **2.2.1** As the cold codegen, when debug info is on, I walk the reachable
  types and fill the table, then write it to the slot sidecar alongside `dbgloc`.
- **2.2.2** As `ValueInspector`, given a type name at a stop, I look up its record
  and decode from the stored offsets/strides — never touching `CajetaType`.
- **2.2.3** As a decoder for a type absent from the table, I return a clean
  `<unknown>`/no-children (never a fault), so an incomplete closure degrades, it
  does not crash.

---

## 3. Serialization into the whole-program slot
### 3.1 Requirements
- **3.1.1** A new slot sidecar path (`WholeProgramSlot::typeinfo()`), written by
  `writeWholeProgramSlot` next to `meta`/`dbgloc`, under the same all-or-nothing
  rule: if the type sidecar can't be written, the slot does not become a
  half-manifest (mirror the `meta`/`dbgloc` failure handling).
- **3.1.2** A schema-major tag in the sidecar; a reader that sees an unknown
  major refuses the table (empty) rather than misreading — the same version
  discipline as the xref stream.
- **3.1.3** On `tryLoadWholeProgramSlot`, load the type sidecar into the global
  table when debug info is requested, beside the `dbgloc` load. A missing or
  unreadable type sidecar is a **slot miss** when `-g` is set (so a stale slot
  from before this feature recompiles once and gains the sidecar), not a silent
  half-load.

### 3.2 Use cases
- **3.2.1** As a warm debug launch, I load the type sidecar and inspection
  behaves exactly as a cold launch — arrays/objects/collections expand, values
  render, leaves edit.
- **3.2.2** As a launch against a pre-feature slot (no type sidecar), the slot
  misses under `-g`, recompiles once, writes the sidecar, and every subsequent
  launch is warm-correct.
- **3.2.3** As a non-debug run (`-g` off), nothing changes — the type sidecar is
  neither written nor required.

---

## 4. ValueInspector reads the table, not the live type world
### 4.1 Requirements
- **4.1.1** `ValueInspector` resolves layout through the global `DebugTypeTable`
  (a small provider seam), not `CajetaType::of`/`CajetaClass`/`CajetaArray`
  directly. The bridge becomes type-world-independent — it works wherever the
  table is populated.
- **4.1.2** The cold path populates the table from the type world and then
  inspects through it, so the cold decode exercises the **same** code as the warm
  decode (closing the test gap: a table-backed decode test is representative of
  the IDE's warm path).
- **4.1.3** Behavior is unchanged from today on the cold path: every existing
  `ValueInspector`/`DapServer` test stays green.

### 4.2 Use cases
- **4.2.1** As a test, I build the table from a compiled program and decode
  through it (cold), and separately load a written sidecar and decode through it
  (warm), asserting identical results for the same program — the warm path is
  finally under test.
- **4.2.2** As `DapServer`, I mint references, answer `variables`, `setVariable`,
  and `evaluate` through the table-backed bridge with no code change to those
  handlers.

---

## 5. Robustness
### 5.1 Requirements
- **5.1.1** Every table lookup tolerates a missing record (unknown/garbage type)
  without faulting (§2.2.3).
- **5.1.2** The offsets are authored cold from DataLayout and consumed against the
  same target; a mismatch (wrong target, corrupt sidecar) fails the version/shape
  check and yields an empty table, never a wrong offset.
- **5.1.3** The closure walk (§2.1.1) that decides which types to serialize logs
  what it bounded, if anything (no silent truncation of reachable types — a type
  that should be inspectable but was dropped is a visible gap, not `<unknown>`
  with no explanation).
