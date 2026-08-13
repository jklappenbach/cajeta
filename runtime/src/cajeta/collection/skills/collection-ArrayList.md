---
id: collection-ArrayList
applies-to: [cajeta/collection/ArrayList]
title: ArrayList — growable T[]-backed sequence
description: cajeta's default mutable list — construction, get/set/add/appendAll, stream/sort, and the array-ownership trap on grow.
---

# ArrayList&lt;T&gt;

The workhorse growable, index-addressable sequence in `cajeta.collection`, backed by a
heap `T[]` that **doubles** on demand. This is the **go-to mutable list** and the default
accumulator behind `Collectors.toList<T>()`. Reach for it whenever you need an ordered,
append-and-index collection. Need an immutable snapshot instead → `ImmutableList`; need
key→value or membership → `HashMap` / `HashSet`.

**Access point:** yes — you construct and drive it directly.

## Construction & ownership

Two constructors. The no-arg one allocates a backing `T[]` of initial capacity 16; the
array-literal one, `ArrayList(#T[] items)`, **consumes** its literal
(`heap ArrayList<int32>([1, 2, 3])`) and sizes the backing array to the element count up
front, so the fill never re-grows. Heap-allocate either.

```cajeta
import cajeta.collection.ArrayList;

ArrayList<int32> xs = heap ArrayList<int32>();
xs.add(10);
xs.add(20);
xs.add(30);                       // count() == 3
int32 first = xs.get(0);          // 10
xs.set(1, 25);                    // [10, 25, 30]
int32 sum = xs.stream().reduce(0, (a, b) -> a + b);   // 55
```

`T` may be primitive (`ArrayList<int32>`) or class-typed (`ArrayList<Point>` holding
`heap Point(...)` elements). `add` takes a plain `T`, so it **lends** by default —
the caller keeps ownership and the list holds the same instances. Ownership is the
caller's choice at the call site: `xs.add(#p)` / `xs.add(#heap Point(...))` transfers,
after which the list owns that slot, drops it at teardown, and `#[]` extraction can
take the title back out. That works because `add` stores with `#=` **in its own frame**;
a wrapper of your own that merely forwards a plain formal on to `add` does *not* pass
the title along — it drops at the wrapper's return and leaves the list holding freed
storage, so lend through such helpers rather than transferring into them.

## Methods that matter

- `int32 count()` — live element count. **The size accessor is `count()`, not `size()`** —
  the field is named `sizeCount` precisely to avoid colliding with this method.
- `boolean isEmpty()` — `count() == 0`.
- `T get(int32 i)` / `void set(int32 i, T v)` — slot read/write; both **throw** an
  `Exception` on an out-of-range index. Returns/stores by `T`'s normal value/reference
  convention (borrowed for class `T`; copy the element if you need to outlive the list).
  A `set` over a slot the list owns drops the displaced element; `set(i, #v)` is the
  clean way to transfer into an existing slot.
- `void add(T v)` — append, growing (doubling) the backing array first if full.
- `void insert(int32 i, T v)` — insert at `i`, shifting `[i, count)` right; `i == count()`
  appends. O(n − i), throws outside `[0, count]`.
- `#T removeAt(int32 i)` — remove at `i`, shifting the tail left, and hand the element
  back **owned**: bind it (`Point p = xs.removeAt(0)`) to keep it, or discard the call and
  the drop fires at the statement end. Throws out of range; panics `TITLE_MISS` if that
  slot's title was already `#[]`-extracted.
- `void clear()` — drop every owned element and reset to the initial capacity; the list
  stays usable.
- `T operator[](int32 i)` / `void operator[]=(int32 i, T v)` — sugar over `get`/`set`.
  **Plain on both sides** (unlike `HashMap`'s `#K`/`#V` subscript write), so `xs[i] = v`
  lends, exactly like `set`. Do **not** write `xs[i] = #v` to transfer: `operator[]=`
  hands `v` on to `set` *plainly*, so the title stops in the subscript frame and frees
  the value there (measured: a drop beyond the displaced element's, and the slot then
  reads garbage). `xs.set(i, #v)` is the clean transferring store.
- `#T operator#[](int32 i)` — title-extracting subscript, `T t #= xs[i]`. The title moves
  out and the slot stays resident but decays to borrowed; extraction from an out-of-range
  index, or from a slot that holds no title (lent, or already extracted), panics.
- `void appendAll(ArrayList<T> other)` — append every element of `other` in order. **Does
  NOT consume `other`** — the caller still owns its list afterward. This is the combiner
  `Collectors.toList<T>()` uses to fold parallel partials.
- `#ArrayStream<T> stream()` — heap `ArrayStream` over `data[0..count-1]`; **ownership
  transfers to the caller** (the `#` return), who drives and drops it. Import
  `cajeta.lang.stream.ArrayStream` (or `Stream`) to hold it. Snapshot over current
  contents — do not mutate the list while a stream is live.
- `void sort()` — in-place ascending natural order (`<` on `T`), unstable quicksort.
- `void sortStable()` — in-place ascending, stable (merge sort; equal elements keep input
  order). Both delegate to `cajeta.collection.Sort` and touch only `data[0..count-1]`;
  trailing spare capacity is untouched. No copy.

## Sharp edge — array ownership on grow

Inside the implementation, `add`'s grow path is the one ownership subtlety, and it is
handled for you: the fresh array is installed with `this.data = #grown` so the local's
scope-exit drop does not free storage still referenced by the field. You inherit this
only if you write similar manual-array code; calling `add` is safe. (The call-site
sharp edge is the subscript write — see `operator[]=` above.)

## Lifecycle & state

Plain heap object — drops under normal scope/ownership rules. There is **no** `close()`
and no manual free. Mutable and **not** thread/fiber-safe; under `.parallel().collect(...)`
each worker gets its own partial `ArrayList` and they are merged via `appendAll`, so the
shared-mutation hazard never arises through that path.

## What it does NOT do (v1)

No `indexOf`/`contains` (search by index with `count()` + `get`), no `size()` — the
accessor is `count()` — and no `Iterator`: traverse by index or through `stream()`.
Removal is by position only (`removeAt`), not by value. The surface is exactly: the two
ctors, `count`/`isEmpty`, `get`/`set`, `add`/`insert`/`removeAt`/`clear`, `appendAll`,
the three subscript operators, `stream`, `sort`/`sortStable`.

See `cajeta.lang.stream` skills for the `ArrayStream`/`Stream` terminal surface
(`reduce`/`fold`/`map`/`collect`), `cajeta.collection.Sort` for the comparator-taking sort
overloads, and `cajeta.collection.Collectors` for `toList<T>()`.
