# RedBlackTree\<K, V\>

`cajeta.collection.RedBlackTree` — ordered map backed by a parent-pointered
red-black tree (CLRS formulation). Keys are kept sorted by `<` / `>` on `K`,
so the tree supports ordered queries (`min`, `max`, in-order key listing) that
a hash map cannot, at O(log n) lookup/insert. Primitive `K` works today; class
`K` needs `operator<` / `operator>`. `get` / `min` / `max` return the type's
zero value when absent or empty; deletion is deferred, so this is an
insert/lookup ordered map.

```cajeta
RedBlackTree<int32, String> idx = heap RedBlackTree<int32, String>();
idx.put(42, "answer");
idx.put(7, "lucky");

String v = idx.get(42);            // "answer"
boolean has = idx.containsKey(7);  // true
int32 lo = idx.min();              // 7
int32 hi = idx.max();              // 42
ArrayList<int32> keys #= idx.keysInOrder();  // ascending
```

## Methods

| Signature | |
|---|---|
| `RedBlackTree()` ⚑ | Create an empty ordered map |
| `int64 count()` | Number of entries |
| `boolean isEmpty()` | `count() == 0` |
| `void put(K key, V value)` | Insert or update the entry for `key` |
| `V get(K key)` | Value bound to `key`, or the type's zero value if absent |
| `boolean containsKey(K key)` | True iff `key` is present |
| `K min()` | Smallest key, or the type's zero value if the tree is empty |
| `K max()` | Largest key, or the type's zero value if the tree is empty |
| `#ArrayList<K> keysInOrder()` | All keys in ascending order, as a fresh heap-allocated `ArrayList<K>` |

⚑ = `@EntryPoint`

## See also

- Tour: [RedBlackTreeDemo](../../../samples/tour/src/main/cajeta/tour/collection/RedBlackTreeDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/RedBlackTree.cajeta`](../../../runtime/src/cajeta/collection/RedBlackTree.cajeta)
- [BPlusTree](BPlusTree.md) — the wide-fanout ordered map
- [ArrayList](ArrayList.md) — backs `keysInOrder`
