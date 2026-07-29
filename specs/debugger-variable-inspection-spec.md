# debugger-variable-inspection — spec

## 1 Definition

### 1.1 Purpose
Make stopped-state variables genuinely inspectable in CLion: structured
expansion of arrays, objects, and collections; readable values (a String shows
its text, a compound shows a summary); declared names at every level; editing of
scalar leaves wherever they sit; and value-on-hover in the editor.

### 1.2 Problem (Julian, live tour session 2026-07-22)
The Variables view shows flat locals only. A local reads:

```
args = [cajeta.lang.String[]]   <cajeta.lang.String[]@0x...>   borrow heap
        └ type column            └ value                        └ facet tag
```

Three defects:
- The type column and the value are the same string twice — the value is the
  type name wrapped in `<…@ptr>` because `formatValue` renders every
  non-primitive that way (`src/cajeta/dbg/DebugVars.cpp:153-157`).
- Nothing expands: `variablesReference` is hardcoded `0`
  (`src/cajeta/dap/DapServer.cpp:72`), so an array's elements, an object's
  fields, and a collection's items are all unreachable.
- A String shows as `<…@ptr>` instead of its text.

### 1.3 Scope
Read and render structured values at a stop; edit scalar leaves; evaluate a
bare identifier (and simple field/element paths) on hover. All five parts the
developer confirmed: browse, render, declared names, edit-through-references,
editor hover.

### 1.4 Non-goals
- Full expression evaluation on hover (method calls, arithmetic, arbitrary
  cajeta expressions). Only identifiers and simple `.field` / `[index]` paths.
- Editing compound values (assigning a whole object/array) or String contents
  (String is immutable; `runtime/src/cajeta/lang/String.cajeta:26-52`).
- A hex/byte memory view — that is the separate `memory-viewer` spec.
- Mutating the program to compute a value (no method invocation at a stop).

### 1.5 Constraints
- **Read live process memory defensively.** At a stop, a slot may hold null, a
  dangling pointer (moved-out binding), or uninitialized bytes. Every decode
  step must tolerate that without crashing the debug server: null → `<null>`,
  moved-out → never dereferenced, unreadable → a rendered marker, never a fault.
- **Bounded work.** Large arrays/collections must page. Object graphs may be
  cyclic (a node pointing back to itself); expansion is lazy per DAP request, so
  depth is naturally user-driven, but a single summary must not recurse without
  a depth bound.
- **Type identity is a string.** The only handle the debugger has on a
  variable's type is the canonical name (`DbgVar.type`, e.g.
  `"cajeta.lang.String[]"`). Layout comes from bridging that string to
  `cajeta::CajetaType::of(name)`, which is live in the JIT process
  (`src/cajeta/jit/CajetaJitHost.cpp:172-186`).
- **Match the runtime layout exactly** (§2). A wrong offset reads a neighbouring
  field as garbage — worse than showing nothing.

---

## 2 Type-introspection bridge

The enabling capability the rest depends on: given a canonical type name and a
live address, decode the aggregate into typed children, and produce a collapsed
summary. Lives compiler-side (needs LLVM `DataLayout` + `CajetaType`), reachable
from the DAP server because both share the JIT process.

### 2.1 Requirements
- **2.1.1** A `type-name → layout` resolver: `CajetaType::of(name)` →
  `CajetaClass`/`CajetaArray` → `getStructLayout` / element stride. Cache per
  name; a stop touches the same handful of types repeatedly.
- **2.1.2** The bridge is the only place that knows a runtime layout. `DebugVars`
  and `DapServer` call it; they never reach into `CajetaType` directly.
- **2.1.3** A decode result is either a **leaf** (primitive or String: a rendered
  value, no children) or an **aggregate** (array/object/collection: a summary
  plus a lazily-enumerable child list). Each child carries `{declaredName,
  typeName, address, storage}` where `storage` distinguishes an inline value slot
  from a pointer slot (§2.2, §2.3).

### 2.2 Use cases — array decode
Array `T[]` is `{ i64 size, [0×T] data }` (`src/cajeta/type/CajetaArray.cpp:93-118`);
length at offset 0, element stride = `DataLayout::getTypeAllocSize` of the built
element type (`CajetaArray.cpp:60-72`).

- **2.2.1** As the bridge, given `("int32[]", addr)`, I read the length word at
  offset 0 and enumerate `length` children `[0..n)`, each a primitive leaf read
  from `data + i*stride`.
- **2.2.2** Given `("cajeta.lang.String[]", addr)`, I enumerate `length`
  children; a String element occupies a 64-byte inline slot with the `String*`
  at the slot base (`runtime/native/cajeta_rt_core.c:1153-1157`), so each child's
  address is the pointer read at `data + i*stride`, typed `cajeta.lang.String`.
- **2.2.3** Given a reference-class array `("tour.Point[]", addr)` where elements
  are 8-byte pointer slots, I read the pointer at each slot base and yield a
  child typed `tour.Point`. Given a value-struct array where elements are stored
  inline, the child address is the slot itself (no indirection).
- **2.2.4** Given an array whose length exceeds the page size, I yield the first
  page and a synthetic `[N more…]` node that pages the remainder, so a
  million-element array never enumerates eagerly. The page size is a launch
  parameter the plugin supplies (§3.1.4); the bridge treats it as given.
- **2.2.5** Given a null or moved-out array pointer, I yield zero children and a
  summary of `<null>` / `<moved-out>`; I never dereference.

### 2.3 Use cases — object decode
A reference class carries a vtable pointer at slot 0; user fields start at LLVM
index 1 (`src/cajeta/type/CajetaClass.h:503-512`). A `@ValueType` POD has no
vtable word; fields start at index 0. Field byte offset comes from the struct's
`DataLayout` (`src/cajeta/field/StructureField.cpp:9-15`).

- **2.3.1** Given `("tour.Point", addr)` for a class with fields `x, y`, I yield
  children `x` and `y` with their declared names, each addressed at
  `addr + offsetof(slot)`, typed by the field's declared type.
- **2.3.2** Given a subclass, I enumerate inherited fields then own fields in
  layout order (`CajetaClass.h:525-536`), each under its declared name.
- **2.3.3** Given a multi-parent class with interior secondary vtable words
  (`CajetaClass.h:684-703`), I skip those words — a field's address is always its
  `DataLayout` offset, never a naive `index*8`.
- **2.3.4** Given a field that is itself an aggregate (a nested object or array),
  I yield it as an expandable child, not a decoded subtree — expansion is lazy,
  one DAP `variables` request per level.
- **2.3.5** Given a `@ValueType` instance, I start fields at offset 0 (no vtable
  skip).

### 2.4 Use cases — String decode
String is immutable UTF-8, tagged: struct `{ vtable, int32 lenTag, int32 aux,
int8[] base, int32 cachedCpLength }`; `lenTag & 0x1FFFFFFF` = byte length;
`len ≤ 12` stores text inline across `{aux, base}`; `len > 12` puts a window
offset in `aux` and a root array header in `base`, text at `base[8+aux..]`
(`runtime/src/cajeta/lang/String.cajeta:26-58`). Field offsets via `DataLayout`
(`CajetaJitHost.cpp:169-189`).

- **2.4.1** Given `("cajeta.lang.String", addr)`, I read `lenTag`, mask the byte
  length, decode the bytes (inline vs windowed per the tag), and return the text
  as a leaf value — no children.
- **2.4.2** Given text with characters needing escaping (newline, quote,
  non-printable), I return an escaped, quoted rendering.
- **2.4.3** Given a null String pointer, I return `<null>`; given a length that
  would read past a plausible bound, I return a truncated value with an ellipsis
  rather than reading unbounded memory.

### 2.5 Use cases — collection logical views
Collections wrap the primitives above. `ArrayList` holds a backing `T[]` and a
size; `HashMap` holds buckets. A raw field view is usable but shows internals; a
logical view shows the elements the user cares about.

- **2.5.1** Given an `ArrayList<T>`, I yield `size` element children `[0..size)`
  from the backing array (not the backing array's full capacity), each typed `T`.
- **2.5.2** Given a `HashMap<K,V>`, I yield one child per live entry, labelled by
  key, valued by the mapped value (bounded/paged like arrays).
- **2.5.3** Given a collection type the bridge has no logical view for, I fall
  back to the object field view (§2.3) so nothing is unexpandable — the user can
  still drill into the backing store by hand.

---

## 3 Structured expansion (DAP surface)

The server must mint expandable references and answer `variables` for them.

### 3.1 Requirements
- **3.1.1** A per-stop handle table maps a non-zero `variablesReference` to a
  decode target `{typeName, address}` (today only frames get references,
  `DapServer.cpp:613-633`). Handles are invalidated when the program resumes —
  addresses are stop-scoped.
- **3.1.2** `variableJson` sets `variablesReference` to a fresh handle when the
  decoded value is an aggregate with children, `0` when it is a leaf
  (`DapServer.cpp:66-93`).
- **3.1.3** A `variables` request on an aggregate handle decodes that aggregate's
  children through the bridge and returns them, each itself a variable that may
  carry its own reference.
- **3.1.4** The expansion page size is configurable in the plugin settings
  (Settings | Languages & Frameworks | Cajeta), default **50** rows. The plugin
  passes it to the server at launch; the server has a hard fallback if it is
  unset or nonsensical. The collapsed ≤5-inline rule (§4.1.3) is a fixed display
  convention, not the page size — the two are independent.

### 3.2 Use cases
- **3.2.1** As a developer stopped at a breakpoint, when I expand `args` in the
  Variables view, I see one row per element with its index and value.
- **3.2.2** When I expand an object local, I see its declared fields; expanding a
  field that is itself an object drills in one level further.
- **3.2.3** When I expand a large array, I see a bounded first page and a node to
  load more; the UI never hangs enumerating the whole thing.
- **3.2.4** When I resume and stop again, stale references from the previous stop
  are gone; expanding uses fresh handles.

---

## 4 Value rendering (collapsed summaries + leaves)

Replaces the `<type@0xADDR>` rendering. Confirmed style: **rich summary**.

### 4.1 Requirements
- **4.1.1** A primitive leaf renders its typed value (unchanged from today).
- **4.1.2** A String renders its quoted, escaped text as the value.
- **4.1.3** An array/collection renders its collapsed value by size: **5 or
  fewer elements** inline the elements — `["build", "--release", "tour"]` —
  each element rendered by its own leaf value (a String quoted, a primitive
  typed); **more than 5** show the count `{N elements}`. An element that is
  itself a compound is shown by its brief summary (§4.1.4) inside the inline
  list, and the whole inline rendering is still length-capped — past the cap it
  falls back to the count even at ≤5.
- **4.1.4** An object renders a brief field peek — the first few scalar fields as
  `{x=3, y=4}` — bounded in length, `{…}` if it has no cheap scalar fields.
- **4.1.5** The type column shows the simple declared type (`String[]`, `Point`),
  not the fully-qualified `<cajeta.lang.String[]@ptr>` form. Type and value no
  longer duplicate each other.
- **4.1.6** Facet tags (alloc/ownership/lifetime) and the moved-out read-only
  treatment are preserved exactly as today (`DapServer.cpp:75-92`,
  `CajetaValue.kt`).

### 4.2 Use cases
- **4.2.1** As a developer, when I look at `name: String`, the value column reads
  `"Ada"`, not a handle.
- **4.2.2** When I look at `args: String[]` (3 elements) collapsed, the value
  reads `["build", "--release", "tour"]`; a `String[]` of 40 elements reads
  `{40 elements}`. The address is never the primary value.
- **4.2.3** When I look at `origin: Point` collapsed, the value reads
  `{x=3, y=4}`; expanding shows `x` and `y` as rows.
- **4.2.4** When a binding is moved-out, its value still renders as an error/
  consumed state and stays read-only — the new rendering does not regress the
  facet semantics.

---

## 5 Declared names at every level

### 5.1 Requirements
- **5.1.1** Array element children are named by index (`[0]`, `[1]`), never by a
  synthetic slot symbol.
- **5.1.2** Object field children use the field's declared name from the class
  definition, never a mangled/linker name.
- **5.1.3** Collection entry children use index (list) or key (map).

### 5.2 Use cases
- **5.2.1** As a developer expanding a `Point`, I see rows `x` and `y` — the
  names as written in source.
- **5.2.2** As a developer expanding a `String[]`, I see `[0]`, `[1]`, `[2]`.

---

## 6 Editing through references

`setVariable` today targets a frame-local scalar by name (`DapServer.cpp:653-671`,
`writeValue` handles primitives only, `DebugVars.cpp:160-200`). Extend to any
scalar leaf reached by expansion.

### 6.1 Requirements
- **6.1.1** A `setVariable` on a child reference resolves the handle to
  `{typeName, address}` and, if the target is a primitive leaf, writes via
  `writeValue` at that address.
- **6.1.2** A non-primitive target (object, array, String) is read-only — the
  server reports it as such; the plugin offers no edit affordance.
- **6.1.3** A moved-out leaf stays read-only (existing rule, §4.1.6).
- **6.1.4** After a successful write, the response carries the re-rendered value
  so the view updates in place (as today).

### 6.2 Use cases
- **6.2.1** As a developer, when I edit `origin.x` to `10`, the field updates and
  the object's summary re-renders `{x=10, y=4}`.
- **6.2.2** When I edit `nums[2]` to `-1`, the element updates in place.
- **6.2.3** When I try to edit a String or an object row, no editor opens — it is
  not presented as editable.

---

## 7 Editor hover evaluation

No `evaluate` request exists today. Add a minimal one plus the plugin evaluator.

### 7.1 Requirements
- **7.1.1** A server `evaluate` request takes an expression string and a frame,
  resolves a **bare identifier** against that frame's locals, and returns the
  same rendered value + `variablesReference` a Variables row would carry.
- **7.1.2** The evaluator additionally resolves a **simple path** — `a.b`,
  `arr[0]` — by decoding through the bridge from the resolved root. It rejects
  anything else (method calls, operators) with a clean "unsupported expression",
  never a guess.
- **7.1.3** The plugin provides an `XDebuggerEvaluator` that sends `evaluate` for
  the hovered identifier and shows the result in the hover popup, expandable if
  it carries a reference.
- **7.1.4** Evaluation is read-only and side-effect free — no program mutation.

### 7.2 Use cases
- **7.2.1** As a developer stopped in a frame, when I hover `args` in the editor,
  a popup shows `{3 elements}`, expandable to the elements.
- **7.2.2** When I hover `origin.x`, the popup shows `3`.
- **7.2.3** When I hover an expression the evaluator does not support, the popup
  says so plainly rather than showing a wrong value.
- **7.2.4** When I hover an identifier not live in the current frame, the popup
  shows nothing (or "not available"), not an error dialog.

---

## 8 Risks and open questions

- **8.1** Multi-parent field offsets (§2.3.3) are the highest-risk decode: a
  wrong offset silently reads a neighbour. Tests must include a class with a
  secondary vtable, not just single-inheritance `Point`.
- **8.2** The bridge runs in the DAP thread while the program is parked. It must
  not take locks the program thread holds, or a decode could deadlock against a
  stopped-but-lock-holding carrier.
- **8.3** Collection logical views (§2.5) couple the debugger to stdlib internal
  layout (ArrayList's backing-array field, HashMap's bucket scheme). If those
  change, the view silently rots. Mitigation: drive them off declared field
  names, and fall back to the object view (§2.5.3) rather than reading raw
  offsets. **Resolved:** ship ArrayList + HashMap logical views (§2.5.1-2.5.2),
  driven off declared field names, as the final and separable unit of work — so
  if the coupling proves fragile it is cuttable without losing arrays, objects,
  editing, or hover. Any collection without a hand-written view degrades to the
  object-field view (§2.5.3).
- **8.4** Page size (§2.2.4, §3.1.4) — default 50 rows, configurable in the
  Cajeta settings screen. Resolved.
- **8.5** Paging references must survive across `variables` requests within one
  stop but be dropped on resume (§3.1.1); a leak here grows per-stop.
