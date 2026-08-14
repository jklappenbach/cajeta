# LtmBPlusTree\<K, V\>

`cajeta.collection.ltm.LtmBPlusTree` — disk-backed, larger-than-memory ordered
map: the same B+ tree shape as `cajeta.collection.BPlusTree`, but every node
lives on disk as a page and is paged in on demand by an `LtmPager`. Only
`capacity` pages are resident at once, so the map can far exceed RAM. The
constructor opens (or creates) the index file at `path`; `keyEnc` / `valEnc`
are the `Encoder`s that serialize keys and values. Keys are ordered by `<` /
`>` on `K`; misses return the type's zero value. Deletion is
tombstone-based: `remove` marks the entry dead in place (no page merging),
every read path skips dead entries, and `compact()` rewrites the index
without its tombstones to reclaim the space. Ordered iteration walks the
leaf chain through the pager (`scan()` / `scanFrom(key)` return an
`LtmCursor`), so a scan far larger than the resident pool pages in and out
as it goes. Always `close()` (or at least `flush()`) to make writes
durable.

```cajeta
public final class I32Codec implements Encoder<int32> {
    public I32Codec() { }
    public #int8[] encode(int32 v) {
        int8[] b = heap int8[4];
        b[0] = (int8) (v & 0xFF);
        b[1] = (int8) ((v >> 8) & 0xFF);
        b[2] = (int8) ((v >> 16) & 0xFF);
        b[3] = (int8) ((v >> 24) & 0xFF);
        return #b;
    }
    public #int32 decode(int8[] bytes) {
        int32 r = (((int32) bytes[0]) & 0xFF)
            | ((((int32) bytes[1]) & 0xFF) << 8)
            | ((((int32) bytes[2]) & 0xFF) << 16)
            | ((((int32) bytes[3]) & 0xFF) << 24);
        return #r;
    }
}

public class LtmExample {
    public void run() {
        Encoder<int32> ke = heap I32Codec();
        Encoder<int32> ve = heap I32Codec();
        LtmBPlusTree<int32, int32> t =
            heap LtmBPlusTree<int32, int32>("/tmp/example.idx", ke, ve, 32, 64);
        t.put(42, 126);
        int32 v = t.get(42);   // 126
        t.close();             // flush + release — durable
    }
}
```

## Methods

| Signature | |
|---|---|
| `LtmBPlusTree(String path, Encoder<K> keyEnc, Encoder<V> valEnc, int32 order, int32 capacity)` ⚑ | Open (or create) the disk index at `path`; a new file is laid out with branching factor `order`, an existing file is reopened from its header (the stored order wins); `capacity` caps the resident pages (it must exceed the tree's depth — a `put` pins its whole root-to-leaf path) |
| `LtmBPlusTree(String path, Encoder<K> keyEnc, Encoder<V> valEnc, BufferEncoder<K> keyFast, BufferEncoder<V> valFast, int32 order, int32 capacity)` | The buffer-encoder form: page serialization writes entries with `encodeInto` — zero allocations per entry; the fast encoders must produce byte-identical output to the `Encoder`s |
| `int64 count()` | Number of LIVE entries (tombstoned keys are not counted) |
| `boolean isEmpty()` | True when no entries have been inserted (no root page yet) |
| `void flush()` | Write all dirty resident pages to disk without closing the file |
| `void close()` | Flush and release the underlying file; call before discarding the index |
| `void put(K key, V value)` | Insert or update the entry for `key`; re-putting a removed key revives it |
| `V get(K key)` | Value bound to `key`, or the type's zero value if absent or removed |
| `boolean containsKey(K key)` | True iff `key` is present and live |
| `boolean remove(K key)` | Tombstone the entry for `key`; true iff a live entry was removed. Durable like any other write; space returns via `compact()` |
| `K min()` | Smallest live key, or the type's zero value if empty |
| `#LtmCursor<K, V> scan()` | Ordered cursor over every live entry, ascending; pages through the pool as it walks |
| `#LtmCursor<K, V> scanFrom(K key)` | Ordered cursor positioned at the first live entry at or after `key` |
| `void compact()` | Rewrite the index without its tombstones (streams through a `path + ".compact"` scratch file, then reopens); bounded memory |

⚑ = `@EntryPoint`

## See also

- Tour: [LtmBPlusTreeDemo](../../../../samples/tour/src/main/cajeta/tour/collection/LtmBPlusTreeDemo.cajeta)
- Source: [`runtime/src/cajeta/collection/ltm/LtmBPlusTree.cajeta`](../../../../runtime/src/cajeta/collection/ltm/LtmBPlusTree.cajeta)
- [BPlusTree](../BPlusTree.md) — the in-memory tree with the same surface
- [Encoder](../../wire/Encoder.md) — the key/value serialization seam
