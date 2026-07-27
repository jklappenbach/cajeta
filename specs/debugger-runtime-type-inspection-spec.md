# debugger-runtime-type-inspection — spec

## 1. Definition

Variable inspection decodes a value row by its **declared** type. Two things a
debugger user expects are therefore invisible:

- **Runtime type.** A `DemoClass`-typed row holding an `AllocationDemo` shows
  `DemoClass`'s fields — for the tour's `demos` list that is *zero* fields, so
  an element row expands to nothing (found live 2026-07-27).
- **Static fields.** `objectChildren` skips `isStatic()` members, so
  `DemoClass.checks`/`failures` — the values the tour demos exist to update —
  never appear.

This spec adds both: resolve each reference row's runtime type from its
instance's vtable pointer and decode the **full inherited-then-own field set of
that runtime type**, and display **static fields inline alongside instance
fields**. Everything must work identically on a cold (cache-miss) and a warm
(cache-hit) launch — the facts ride the `DebugTypeTable` and its
`program.typeinfo` sidecar (debug-type-sidecar), never the live type world.

### 1.1 Scope
- Runtime-type resolution for every reference-class value row: locals, object
  fields, array elements, collection elements, hover/evaluate results.
- The vtable→type mapping serialized so it survives a cache hit.
- The type-table closure widened so any runtime type resolves warm.
- Static fields as child rows, readable and editable, cold and warm.

### 1.2 Non-goals
- No grouping UI for statics (no "static members" node) — they render inline;
  plugin styling via the DAP presentation hint only.
- No runtime-type narrowing for **value types** — they have no vtable word and
  their declared type is exact.
- No reflection/RTTI general facility; decode-time reverse lookup only.
- No change to breakpoints, stepping, frames, or existing sidecars.

### 1.3 Fixed facts (verified in source, 2026-07-27)
- A reference-class instance carries its vtable pointer at slot 0
  (`hasVtablePointerAtSlotZero`); value types carry none.
- The primary vtable is a global named `<canonical>#VTable`. A multi-parent
  class emits **separate named secondary vtables**
  `<canonical>$as$<parent>#VTable`, and a base-adjusted reference points at the
  sub-object whose slot-0 word is that secondary vtable
  (`getOrCreateSecondaryVTable`, `getSubObjectByteOffset`).
- A static field is a global named `<canonical>.<fieldName>`
  (`getOrCreateStaticFieldGlobal`).
- Vtable and static-field globals live in the cached bitcode, so their symbols
  resolve by name through the LLJIT on a hit; **addresses are per-run, symbols
  are stable** — the sidecar carries symbols, the session resolves addresses.

---

## 2. Runtime-type resolution

### 2.1 Requirements
- **2.1.1** The type table carries a **vtable map**: one entry per vtable global
  — `{symbol, canonicalType, subObjectByteOffset}` — where the primary vtable
  has offset 0 and each secondary (`$as$`) vtable carries its sub-object's
  offset within the instance.
- **2.1.2** At session start (cold or warm), the debug host resolves each vtable
  symbol to its runtime address once, building the address→entry map decode
  uses. A symbol that fails to resolve is dropped from the map (its type
  degrades to declared-type decode), never a launch failure.
- **2.1.3** Decoding a reference-class row reads the instance's slot-0 word and
  looks it up: on a match, the row's type becomes the entry's canonical type
  and the instance pointer is **rebased by −subObjectByteOffset** before field
  offsets apply, so a base-view pointer into a multi-parent object decodes the
  whole object correctly.
- **2.1.4** An unmatched slot-0 word (garbage, torn object, foreign pointer,
  dropped symbol) falls back **silently** to declared-type decode — never a
  fault, never a guessed layout.
- **2.1.5** The resolved runtime type drives everything downstream: the row's
  reported type string, its collapsed summary (field peek), its children (the
  runtime type's inherited-then-own fields), and nested expansion.
- **2.1.6** Value-type and primitive rows are untouched (declared type is
  exact). String rows keep their Leaf/ABI decode.

### 2.2 Use cases
- **2.2.1** As a user stopped in the tour, when I expand `demos` and then an
  element row, the row shows type `AllocationDemo` (not `DemoClass`) and lists
  the full field set of `AllocationDemo` — inherited fields first, then own.
- **2.2.2** As a user inspecting a `C extends A, B` object through a `B`-typed
  reference, the fields of the whole `C` object decode at their correct
  offsets (the interior pointer is rebased), including `A`'s and `C`'s own
  fields.
- **2.2.3** As a user whose slot points at freed/garbage memory, the row
  degrades to declared-type decode with no crash and no misread.
- **2.2.4** As a warm (cache-hit) launch, all of the above behaves identically
  to the cold launch of the same program.

---

## 3. Closure: carry the whole compiled world

### 3.1 Requirements
- **3.1.1** `buildFromTypeWorld` carries a record for **every class in the
  compiled program** (user and stdlib), not just the declared-local closure —
  a `DemoClass`- or `Object`-typed row can hold any subtype, and the table
  must answer for whichever it holds, warm.
- **3.1.2** The existing bound + logged-drop machinery (`BuildOptions`,
  `bounded()`) still applies; the bound is raised to fit the full world and a
  drop remains visible, never silent.
- **3.1.3** The sidecar format is unchanged in shape (records + abi + the new
  vtable-map lines); the schema major bumps only if a v1 reader would misread
  — additive line kinds require a major bump because the v1 reader refuses
  unknown line kinds by design.

### 3.2 Use cases
- **3.2.1** As a warm launch, a row whose runtime type was never named in any
  local declaration (reached only via the vtable map) still expands with full
  fields.
- **3.2.2** As a developer reading launch logs after the world outgrows the
  bound, I see which types were dropped.

---

## 4. Static fields

### 4.1 Requirements
- **4.1.1** A class record carries its **static fields**:
  `{name, typeName, symbol}` (symbol = `<canonical>.<fieldName>`), populated
  cold from the type world, serialized in the sidecar.
- **4.1.2** At session start the host resolves each static symbol to its
  runtime address once; an unresolvable symbol drops that row (never a launch
  failure).
- **4.1.3** Expanding an object row lists instance fields first (layout
  order), then the runtime type's statics — inherited-then-own, matching the
  instance-field convention. Each static row carries the DAP
  `presentationHint.attributes: ["static"]` so the plugin can style it; no
  separate group node.
- **4.1.4** A static row decodes exactly like a local of its type (same
  leaf/aggregate rules, same paging) and is **editable** through the existing
  `setVariable` leaf-write path — its address is its resolved global.
- **4.1.5** Statics appear under the **runtime** type of the row (2.1.5): a
  `DemoClass`-typed row holding an `AllocationDemo` shows the statics visible
  on `AllocationDemo` (which includes `DemoClass`'s inherited statics).

### 4.2 Use cases
- **4.2.1** As a user stopped in the tour, expanding a `demos` element shows
  `checks` and `failures` with their live values beside the instance fields.
- **4.2.2** As a user, I edit `failures` at a stop and the program continues
  with the new value.
- **4.2.3** As a warm launch, statics read and edit identically to cold.

---

## 5. Robustness

### 5.1 Requirements
- **5.1.1** Every new lookup (vtable word, static symbol) tolerates a miss
  without faulting; degradation is always to today's behavior, never below it.
- **5.1.2** The vtable map and static symbols are authored cold and consumed
  by symbol, so a rebuilt binary (new slot key) regenerates them and a stale
  sidecar can never map an address to the wrong type.
- **5.1.3** All existing inspection behavior — primitives, String, arrays,
  collections, edit, hover — is unchanged where runtime type equals declared
  type.
