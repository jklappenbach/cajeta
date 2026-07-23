# collection-literals — collection, map, and aggregate literals

## 1. Definition

### 1.1 Purpose
Give Cajeta one coherent literal grammar for building collections and aggregates
inline, resolving the two-bracket split into a clean division of labor:

- **`[ … ]`** builds a **collection** — array, list, set, or map.
- **`{ … }`** builds one **aggregate** — a struct/record value.

The two compose: an array of aggregates is `[ {…}, {…} ]`, a map to aggregates is
`[ "k": {…} ]`. `[…]` never means "struct" and `{…}` never means "collection".

### 1.2 Problem
After `array-literals`, `[…]` is a working array value with target-typed and unify
inference. But:
- **1.2.1** Lists, sets, and maps have no literal — they need `new` + repeated
  `.add`/`.put`. There is no map literal at all.
- **1.2.2** Aggregate init requires a type prefix (`Point{ x: 1, y: 2 }`) even where
  the type is obvious from context, so an array of Points can't be written
  `[ {x:1, y:2}, … ]`.
- **1.2.3** The array `{ … }` initializer (Java-inherited) overlaps `[…]` in
  declarator position, so `int32[] xs = {1,2,3}` and `= [1,2,3]` are redundant
  spellings — the wart this spec also resolves.

### 1.3 Scope
- **1.3.1** Target-typed collection literals for the sequence collections: a `[…]`
  bound to a `List`/`Set` type builds that collection (§2).
- **1.3.2** Map literals: `[ k: v, … ]` and the empty `[:]` (§3).
- **1.3.3** Type-inferred aggregate literals: `{ field: v, … }` with no type prefix,
  the type taken from context (§4).
- **1.3.4** Composition/nesting of the above (§5).
- **1.3.5** Reserve `{…}` for aggregates and migrate array-`{…}` to `[…]`, closing
  the redundancy (§6).

### 1.4 Non-goals
- **1.4.1** Depends on `array-literals` being delivered — this spec builds on its
  `[…]` lowering, target-typing, and the from-array collection constructors
  (array-literals §6 / Unit 5). It does not re-derive them.
- **1.4.2** No comprehension/generator syntax (`[x*2 for x in …]`).
- **1.4.3** No ordering guarantees beyond each collection's own contract (a
  `HashMap` literal does not promise insertion order).
- **1.4.4** No new runtime layout — collection literals lower through existing
  constructors; map literals through a `Pair<K,V>[]` constructor.
- **1.4.5** No implicit collection-type default beyond the two named in §3.4 (an
  ambiguous no-target literal is an error, not a guess).

### 1.5 Constraints
- **1.5.1** A collection literal lowers to an ordinary `[…]` array plus the target
  collection's from-array constructor: `[a,b,c]` → `T[]` → `ArrayList<T>(T[])`.
  Nothing bypasses the array path.
- **1.5.2** A map literal lowers to a `Pair<K,V>[]` array plus a `HashMap<K,V>(Pair<K,V>[])`
  constructor. Keys/values are ordinary element expressions.
- **1.5.3** `[…]` disambiguates by a single-token lookahead: a `:` after the first
  entry expression makes it a map; otherwise it is an array/list/set. `[]` is an
  empty array/sequence; `[:]` is an empty map (Swift's rule).
- **1.5.4** Aggregate-type inference reuses the array-literals target-typing spine
  (declaration/assignment/return/element/param context), extended one level: an
  array's element type or a map's value type flows into a `{…}` element.

## 2. Collection literals (list / set)

### 2.1 Requirements
A `[…]` whose target type is a sequence collection builds that collection by
lowering the literal to a `T[]` and passing it to the collection's `(T[])`
constructor (array-literals §6). Placement (`heap`/`stack`/`shared`) is the
literal's placement (array-literals §4); the collection's own storage follows the
declaration.

### 2.2 Use cases
- **2.2.1** As a developer, `ArrayList<int32> xs = [1, 2, 3];` gives a list of
  1,2,3 in order.
- **2.2.2** As a developer, `HashSet<int32> s = [1, 2, 2, 3];` gives {1,2,3}.
- **2.2.3** As a developer, `LinkedList<String> ns = ["a", "b"];` preserves order.
- **2.2.4** As a developer, a `[…]` with no target and no collection context stays
  an array (array-literals behavior — unchanged).

## 3. Map literals

### 3.1 Requirements
`[ k1: v1, k2: v2 ]` builds a map: each entry is a `key : value` pair, lowered to a
`Pair<K,V>[]` and passed to `HashMap<K,V>(Pair<K,V>[])`. `[:]` is the empty map.
Key and value types come from the target map type, or by unify over the entries
when there is a common type.

### 3.2 Use cases
- **3.2.1** As a developer, `HashMap<String,int32> m = ["blah": 123, "x": 4];`
  gives the two mappings.
- **3.2.2** As a developer, `HashMap<String,int32> e = [:];` is an empty map.
- **3.2.3** As a developer, `["a": 1]` in a `HashMap`-typed context parses as a map;
  `[1, 2, 3]` in the same context parses as a sequence — the colon is the signal.
- **3.2.4** As a developer, a bare `["a": 1]` with no target type builds a
  `HashMap` by default (§3.4); a bare `[]` (no colon) is an array, not a map.

### 3.3 Requirements — parsing
- **3.3.1** The parser decides map-vs-sequence by a single-token lookahead: after
  the first entry's expression, a `:` means map. Empty forms: `[]` sequence,
  `[:]` map.

### 3.4 Requirements — no-target defaults
- **3.4.1** A colon-bearing literal with no target defaults to `HashMap<K,V>` with
  K,V unified from the entries.
- **3.4.2** A colon-free literal with no target stays an array (array-literals).

## 4. Type-inferred aggregate literals

### 4.1 Requirements
`{ field: value, … }` with no type prefix builds an aggregate of the type demanded
by context: the declared/assigned/returned type, an enclosing array's element type,
a map's value (or key) type, or a parameter type. The explicit `Type{ … }` form
keeps working. A `{…}` whose type cannot be inferred and has no prefix is an error.

### 4.2 Use cases
- **4.2.1** As a developer, `Point p = { x: 32, y: 54 };` builds a Point.
- **4.2.2** As a developer, `Point[] pts = [ {x:32, y:54}, {x:1, y:2} ];` builds an
  array of Points; each element's type is the array's element type.
- **4.2.3** As a developer, `HashMap<String,Point> m = [ "origin": {x:0, y:0} ];`
  infers the aggregate type from the map's value type.
- **4.2.4** As a developer, `{ x: 1, y: 2 }` with no inferable target type is a
  compile error naming the missing type.
- **4.2.5** As a developer, `Point{ x: 1, y: 2 }` (explicit prefix) is unchanged.

## 5. Composition

### 5.1 Requirements
The three forms nest by their bracket rules with no special cases: collection of
aggregates, map to aggregates, collection of collections, map to collections.

### 5.2 Use cases
- **5.2.1** `ArrayList<Point> ps = [ {x:1,y:2}, {x:3,y:4} ];`
- **5.2.2** `HashMap<String,int32[]> g = [ "a": [1,2], "b": [3,4] ];`
- **5.2.3** `Point[][] grid = [ [ {x:0,y:0} ], [ {x:1,y:1} ] ];`

## 6. Resolving the `{…}` / `[…]` split

### 6.1 Requirements
With `[…]` covering every collection, `{…}` is reserved for aggregates. The
array-`{…}` initializer becomes redundant and is retired: it keeps parsing with a
deprecation path, stdlib and tests migrate to `[…]`, and it is removed once no
in-tree source uses it. Positional `{1,2,3}` (no `field:`) is the only ambiguous
case and it resolves to the deprecated array form until removal; `{field: …}` is
always an aggregate.

### 6.2 Use cases
- **6.2.1** As a maintainer, after migration every array literal in the tree is
  `[…]` and `{…}` appears only as an aggregate — one bracket per job.
- **6.2.2** As a developer, an old `int32[] xs = {1,2,3}` still compiles during the
  deprecation window (with a warning), so the migration is not a hard break.

### 6.3 Requirements — sequencing
- **6.3.1** §6 migration lands last, after §2–§5 give `[…]` full coverage, so no
  source is stranded between the two forms.
