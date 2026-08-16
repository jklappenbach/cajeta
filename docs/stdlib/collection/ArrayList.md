# ArrayList\<T\>

`cajeta.collection.ArrayList` — growable, index-addressable sequence of `T`,
backed by a heap-allocated `T[]` that doubles its capacity on demand. The
workhorse list of the collections package and the default accumulator behind
`Collectors.toList<T>()`.

```cajeta
ArrayList<int32> xs = heap ArrayList<int32>();
xs.add(10);
xs.add(20);
xs.add(30);
int32 n = xs.count();        // 3
xs.set(1, 25);               // [10, 25, 30]
ArrayStream<int32> s #= xs.stream();
```

## Methods

| Signature | |
|---|---|
| `ArrayList()` ⚑ | Construct an empty list |
| `int32 count()` | Live element count |
| `boolean isEmpty()` | `count() == 0` |
| `T get(int32 i)` | Element at `i` (no bounds check) |
| `void set(int32 i, T v)` | Replace the element at `i` |
| `void add(T v)` | Append, doubling the backing array when full |
| `void appendAll(ArrayList<T> other)` | Append every element of `other`; the parallel-stream combiner |
| `#ArrayStream<T> stream()` | Walk the live elements as a `Stream` |
| `void sort()` | In-place ascending sort, unstable (see [Sort](Sort.md)) |
| `void sortStable()` | In-place merge sort — equal elements keep input order |

⚑ = `@EntryPoint`

## See also

- Tour: [ArrayListDemo](../../../samples/tour/src/main/cajeta/tour/collection/ArrayListDemo.cajeta),
  [StreamsDemo](../../../samples/tour/src/main/cajeta/tour/collection/StreamsDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/ArrayList.cajeta`](../../../runtime/src/cajeta/collection/ArrayList.cajeta)
