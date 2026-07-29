# Heap\<T\>

`cajeta.collection.Heap` — array-backed binary heap / priority queue. A
min-heap: `peek()` and `pop()` return the smallest element by the `<` ordering
on `T`, with sift-up on `push` and sift-down on `pop`, both O(log n). The
backing array grows by doubling like `ArrayList`. `peek` / `pop` on an empty
heap return the type's zero value rather than throwing. Primitive `T` works
today; class `T` needs an `operator<` overload.

```cajeta
Heap<int32> h = heap Heap<int32>();
h.push(5);
h.push(1);
h.push(3);
int32 lo = h.peek();        // 1  (smallest, not removed)
int32 a = h.pop();          // 1
int32 b = h.pop();          // 3
int64 left = h.count();     // 1
boolean done = h.isEmpty(); // false
```

## Methods

| Signature | |
|---|---|
| `Heap()` ⚑ | Create an empty heap with an initial capacity of 16 |
| `int64 count()` | Number of elements currently in the heap |
| `boolean isEmpty()` | `true` when the heap holds no elements |
| `void push(T v)` | Insert `v`, sifting it up to its ordered position |
| `T peek()` | Smallest element without removing it, or the type's zero value if empty |
| `T pop()` | Remove and return the smallest element |

⚑ = `@EntryPoint`

## See also

- Tour: [HeapDemo](../../../samples/tour/src/main/cajeta/tour/collection/HeapDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/Heap.cajeta`](../../../runtime/src/cajeta/collection/Heap.cajeta)
- [ArrayList](ArrayList.md) — the same doubling growth strategy
- [Sort](Sort.md) — one-shot ordering of a whole array
