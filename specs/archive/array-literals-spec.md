# array-literals — array literal expressions and collection initialization

## 1. Definition

### 1.1 Purpose
Make the bracketed literal `[e1, e2, …]` a real value-producing expression that
allocates and populates an array, and give the core collections a way to be built
from one. Today `[…]` parses to `ArrayLiteralExpression` but its `generateCode`
throws `CAJETA_ERROR_NOT_IMPLEMENTED`; the node is read only by the XPU launch-dim
path, which walks the AST directly and never lowers it.

### 1.2 Problem
There is no way to write an array's contents inline as an expression. Arrays are
built by `new T[n]` (`ArrayCreatorRest`) or by the declarator-only brace form
`{…}` (`ArrayInitializer`, which requires the caller to supply `elementType`).
Collections are built only by `new` + repeated `.add(…)`. This is verbose, and the
missing literal blocks other work (the debugger's `ValueInspector` array-decode
unit has no ergonomic way to produce a test array).

### 1.3 Scope
- `[…]` as a general expression that produces an array value (§2).
- Element-type determination: target-typed with a unify fallback (§3).
- Storage placement: heap by default, `stack` / `shared` prefixes to opt out (§4).
- Nested literals for multi-dimensional arrays (§5).
- From-array constructors on the sequence collections so a literal initializes a
  collection (§6).

### 1.4 Non-goals
- **1.4.1** No map/dictionary literal syntax (`{k: v}`). Maps are initialized by
  constructor as today; a from-pairs constructor is out of scope for this pass.
- **1.4.2** No target-type propagation *into* overload resolution. In argument
  position a literal is typed by unify only (§3.3); the resulting concrete array
  type participates in normal overload resolution. `int64[]`-parameter calls that
  receive an integer-literal array are not silently widened.
- **1.4.3** No varargs (`T…`) parameters. From-array constructors take an explicit
  `T[]`.
- **1.4.4** No change to the XPU launch-dim path, which keeps reading elements off
  the AST.
- **1.4.5** No comprehension / generator syntax.

### 1.5 Constraints
- **1.5.1** Reuse the existing array runtime: the `{ i64 size, [0×T] data }` header,
  `__cajeta_new_array_header` / `_bits` / `_arena` allocators, and
  `CajetaArray::elementStrideBytes()` for slot stride. No new runtime layout.
- **1.5.2** Element stores must honour the element type's move/copy/borrow
  semantics exactly as the working `new T[n]` and `{…}` paths do — including the
  per-slot ownership tail bitmap (`_bits` allocator) for droppable element types.
- **1.5.3** The `[…]` lowering and the existing `{…}` `ArrayInitializer` lowering
  share one code path (§7), so both forms behave identically.

## 2. The literal expression

### 2.1 Requirements
`ArrayLiteralExpression::generateCode` allocates an array header sized to the
element count, stores each evaluated element into its slot with the correct width
coercion and ownership handling, and yields the array pointer as the expression's
value. The element type is resolved per §3; the storage per §4.

### 2.2 Use cases
- **2.2.1** As a developer, when I write `int32[] xs = [10, 20, 30];`, then `xs` is a
  heap `int32[]` of length 3 holding 10, 20, 30.
- **2.2.2** As a developer, when I write `String[] names = ["ann", "bo"];`, then each
  slot holds a `String` reference built from the literal, in declaration order.
- **2.2.3** As a developer, when I pass `[a, b, c]` where `a`,`b`,`c` are owned
  objects, then each is moved into its slot and the array owns them (droppable-bits
  allocator), matching `new T[3]` + element stores.
- **2.2.4** As a developer, when I write an empty `[]` with no target type, then I
  get a compile error telling me the element type can't be inferred (§3.4).
- **2.2.5** As the debugger's `ValueInspector` array-decode tests, when a program
  declares `int32[] nums = [10, 20, 30];`, then a real populated array exists at a
  stop to decode (unblocks debugger-variable-inspection Unit 2).

## 3. Element-type determination

### 3.1 Requirements
The element type is fixed at compile time by one of two mechanisms, tried in order.

### 3.2 Target-typed (preferred)
- **3.2.1** When the literal appears in a context that supplies an expected array
  type — a local/field declaration LHS, an assignment LHS, or a `return` whose
  method has a declared array return type — the element type is that context's
  element type, and each element expression is coerced to it.
- **3.2.2** As a developer, when I write `int64[] xs = [1, 2, 3];`, then the elements
  are widened to `int64` (target wins over the narrower unified `int32`).

### 3.3 Unify fallback
- **3.3.1** With no target type, the element type is the least-upper-bound of the
  element expression types: numeric elements promote to the widest numeric among
  them; reference elements resolve to their nearest common superclass. `stack xs =
  [1, 2, 3];` yields a `stack int32[]`.
- **3.3.2** As a developer, when I write `f([1, 2, 3])` (argument position, no target
  propagation), then the literal is typed `int32[]` by unify and overload
  resolution proceeds on that concrete type (§1.4.2).

### 3.4 Failure cases
- **3.4.1** Empty `[]` with no target type → compile error (nothing to unify).
- **3.4.2** Elements with no common type (e.g. two unrelated classes sharing only
  multiple interfaces) and no target type → compile error naming the conflict.

## 4. Storage placement

### 4.1 Requirements
- **4.1.1** A bare `[…]` allocates on the heap (`__cajeta_new_array_header`, or
  `_bits` for droppable elements).
- **4.1.2** A placement prefix selects storage: `stack [1, 2, 3]` uses the frame
  arena (`_arena` where eligible); `shared [1, 2, 3]` uses shared allocation. This
  adds `arrayLiteral` as a placement target in the grammar, beside
  `creator | aggregateInitializer`.

### 4.2 Use cases
- **4.2.1** As a developer, when I write `stack int32[] xs = stack [1, 2, 3];`, then
  the array lives in the frame arena and is not heap-registered.
- **4.2.2** As a developer, when I write `[1, 2, 3]` with no prefix, then it is a
  heap array (default), consistent with `new`.

## 5. Nested literals

### 5.1 Requirements
A literal whose elements are themselves array literals produces a nested
`CajetaArray` (`T[][]`), allocating the outer header of pointers and each inner
array, recursively.

### 5.2 Use cases
- **5.2.1** As a developer, when I write `int32[][] grid = [[1, 2], [3, 4]];`, then
  `grid` is a length-2 array of `int32[]`, each length 2.
- **5.2.2** As a developer, when inner literals have differing lengths
  (`[[1], [2, 3]]`), then the result is a jagged `int32[][]` (each inner sized to
  its own literal).

## 6. Collection initialization

### 6.1 Requirements
The sequence collections gain a constructor taking `T[]`, so an array literal
initializes them. The literal is an ordinary array (§2); the collection copies its
elements in. Placement of the collection stays explicit via the existing keyword.

### 6.2 Use cases
- **6.2.1** As a developer, when I write `heap ArrayList<int32>([1, 2, 3])`, then I
  get a heap `ArrayList` containing 1, 2, 3 in order.
- **6.2.2** As a developer, when I write `heap HashSet<int32>([1, 2, 2, 3])`, then I
  get a set of {1, 2, 3}.
- **6.2.3** The from-array constructor is added to `ArrayList`, `LinkedList`,
  `HashSet`, `ImmutableList`, and `ImmutableSet`. `HashMap` is unchanged (§1.4.1).

## 7. Shared lowering

### 7.1 Requirements
- **7.1.1** Factor the element-store loop (allocate header → per-slot GEP → coerced
  store → ownership bits) into one helper used by both `ArrayLiteralExpression`
  (§2) and the existing `ArrayInitializer` `{…}` codegen, so the two forms cannot
  drift.

### 7.2 Use cases
- **7.2.1** As a maintainer, when I fix an element-store bug, then both `[…]` and
  `{…}` inherit the fix from the shared helper.
