# BPlusTree\<K, V\>

`cajeta.collection.BPlusTree` — in-memory ordered map backed by a B+ tree: all
entries live in leaves, internal nodes hold only separator keys, and leaves are
singly linked in ascending order for cheap range scans. Keys are ordered by `<`
/ `>` on `K` (primitive `K` works today; class `K` needs `operator<` /
`operator>`). Misses return the type's zero value; deletion is deferred for
this in-memory tree, so it is an insert/lookup ordered map — the disk-backed
[LtmBPlusTree](ltm/LtmBPlusTree.md) has tombstone-based `remove`, ordered
`scan`/`scanFrom` cursors, and `compact()`.

```cajeta
BPlusTree<int32, String> index = heap BPlusTree<int32, String>();
index.put(42, "answer");
index.put(7, "lucky");
String hit = index.get(42);            // "answer"
boolean known = index.containsKey(9);  // false, miss -> zero value
int32 lo = index.min();                // 7
ArrayList<int32> keys = index.keysInOrder();  // ascending
```

## Methods

| Signature | |
|---|---|
| `BPlusTree()` ⚑ | Create an empty tree with a branching `order` of 32 |
| `int64 count()` | Number of entries |
| `boolean isEmpty()` | True when no entries have been inserted |
| `void put(K key, V value)` | Insert or update the entry for `key` |
| `V get(K key)` | Value bound to `key`, or the type's zero value if absent |
| `boolean containsKey(K key)` | True iff `key` is present |
| `K min()` | Smallest key, or the type's zero value if empty |
| `#ArrayList<K> keysInOrder()` | All keys in ascending order, walking the linked leaves |

⚑ = `@EntryPoint`

## See also

- Tour: [BPlusTreeDemo](../../../samples/tour/src/main/cajeta/tour/collection/BPlusTreeDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/BPlusTree.cajeta`](../../../runtime/src/cajeta/collection/BPlusTree.cajeta)
- [RedBlackTree](RedBlackTree.md) — the binary-search-tree ordered map
- [LtmBPlusTree](ltm/LtmBPlusTree.md) — the disk-backed, larger-than-memory variant
- [ArrayList](ArrayList.md) — backs `keysInOrder`
