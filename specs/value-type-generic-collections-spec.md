# value-type-generic-collections — spec

## 1. Definition

Make a value type (a `record` or `@ValueType` class) work as the element type
argument `T` of the stdlib generic collections — `ArrayList<T>`, `HashSet<T>`,
`LinkedList<T>`, `ImmutableList<T>`, `ImmutableSet<T>`, and as `V` (and `K`) in
`HashMap<K,V>`. Today instantiating most of these with a value-type `T` fails to
compile: the template body spells operations (`#v` transfer, `<` ordering,
field access on a by-value `T`) that the codegen mishandles for value types.

### 1.1 Purpose
`ArrayList<Point> ps = [...]` (collection-literals spec §5.2.1) and any program
that stores value types in a collection. Value types are the language's
zero-overhead aggregate (inline storage, Copy semantics); a collection that
can't hold them forces boxing into a reference class.

### 1.2 Scope
The stdlib collections above, instantiated with a value-type `T`/`V`/`K`:
construct, `add`/`put`, `get`, `contains`/`containsKey`, iterate, and `sort`
(sequence collections). The fixes are in the COMPILER (value-type codegen /
calling convention) and, where a template body is genuinely value-type-hostile,
in the stdlib source.

### 1.3 Non-goals
- Ordering of records whose fields are themselves value types / `String` /
  `Utf8` / arrays (the default `operator<` is primitive-field-only; nested
  value-type ordering is future work).
- Value-type `K` in `HashMap` beyond what `.hash()`/`==` already support.
- New collection types.

### 1.4 Status — already delivered
Three facets of this were fixed while it surfaced under collection-literals:
- **1.4.1** Value-type inline reads (a value-type field, or a value-type array
  element) were loaded AS a pointer and dereferenced → SIGSEGV. Fixed in the
  four load sites (`loadIfLValue` + `ReturnStatement`, Dot + ArrayIndex).
  Commit 85cd0ab8. Guard: `test/expression/ValueTypeInlineReadTests.cpp`.
- **1.4.2** Value types had no default ordering; `<` on a value type with no
  `operator<` crashed (ICmp-on-struct), and `ArrayList<T>.sort()` (eagerly
  instantiated, needs `<`) blocked every value-type `T`. Now a record with
  all-primitive fields synthesizes a lexicographic `operator<`. Commit 334d1f6f.
  Guard: `test/expression/ValueTypeOrderingTests.cpp`.
- **1.4.3** `#v` on a value-type param wrongly tripped the borrow-escape check
  (value types are Copy, so `#v` is a no-op copy). Exempted. Commit 334d1f6f.
  Guard: `ValueTypeInlineReadTests.genericValueTypeMoveForward`.
- **1.4.4** Value-type param field-access ABI (§2.2, the GEP-on-value verify
  failure): resolved upstream by the nucleo merge's Method.cpp parameter-ABI
  rework plus the existing DotExpression value-type-receiver alloca spill — the
  by-value param is materialized before its field GEP. Verified 2026-07-24; no
  further code change needed. Guard: `ValueTypeCollectionTests` (3/3:
  `arrayListValueTypeAddGet`, `arrayListValueTypeSort`,
  `staticValueTypeParamFieldAccess`).

## 2. The remaining defect(s)

### 2.1 Requirements
Instantiating `ArrayList<Point>` (value-type `Point`) must compile and run its
basic operations, and `sort()` must order by the default `operator<`.

### 2.2 Known facet — value-type param field access under by-value ABI — RESOLVED (§1.4.4)
- **2.2.1** After 1.4.x, `ArrayList<Point>` reaches a `JIT verify failed: GEP
  base pointer is not a vector` — the instantiated `operator<` / `operator==`
  body GEPs a BY-VALUE `Point` struct (`getelementptr %Point %v, 0, 0` on an
  SSA value, not a pointer) when `sort()` calls it with loaded `Point` values.
  The same `operator<` compiles correctly when called standalone with aggregate
  rvalues (`ValueTypeOrderingTests` passes), so the fault is in how a value-type
  PARAMETER is passed / accessed in this instantiation context: field access on
  a by-value value-type param must first materialize it (spill to an alloca)
  before GEPing, rather than GEP the SSA struct.

### 2.3 Requirements — discovery
- **2.3.1** This is a CHAIN: each facet fixed so far revealed the next. The work
  must instantiate each target collection with a value-type `T` and fix whatever
  surfaces, iterating until a clean compile + green basic-ops run, rather than
  assuming 2.2 is the last one.

## 3. Use cases
- **3.1** `ArrayList<Point>`: construct, `add`, `get`, `count`, `sort` (by
  default order), read back — all compile and run.
- **3.2** `HashSet<Point>` / `ImmutableSet<Point>`: dedup by `==`, `contains`.
- **3.3** `LinkedList<Point>` / `ImmutableList<Point>`: ordered store + `get`.
- **3.4** `HashMap<String,Point>`: already works (1.4.1); keep it green.
- **3.5** The collection-literals forms over value types: `ArrayList<Point> =
  [{x:1,y:2}, ...]` (collection-literals §5.2.1) compiles and runs.
