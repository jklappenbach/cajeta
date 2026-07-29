# Cache\<K, V\>

`cajeta.collection.Cache` — bounded in-memory cache with LRU eviction and
optional TTL. `get` / `put` / `remove` / `containsKey` are amortized O(1)
(one hashmap lookup plus a doubly-linked-list pointer reshuffle); `put`
inserts at the MRU head and evicts the LRU tail when over `maxEntries`.
`setMaxAge` arms time-based expiration: an expired entry misses on lookup
even if still physically present, and `evict()` drains everything expired.
Not thread-safe — wrap in a lock when instances are shared.

```cajeta
Cache<int64, String> cache = heap Cache<int64, String>(1024);
cache.setMaxAge(Duration.ofSeconds(30));

cache.put(42, "answer");

Optional<String> hit = cache.get(42);
if (hit.isPresent()) {
    String v = hit.get();
}
```

## Methods

| Signature | |
|---|---|
| `Cache(int32 maxEntries)` ⚑ | Build a cache with the given LRU bound |
| `void setMaxAge(Duration maxAge)` | Set the maximum age before an entry expires; 0 (the default) disables TTL |
| `int32 count()` | Live entry count |
| `boolean isEmpty()` | `count() == 0` |
| `boolean containsKey(K key)` | True when `key` is present AND not expired |
| `Optional<V> get(K key)` | Look up `key`; present hit is promoted to most-recently-used |
| `void put(#K key, V value)` | Insert (or replace) `key`'s value |
| `void remove(K key)` | Drop `key` and its LRU node if present; a no-op otherwise |
| `void clear()` | Drop every entry and reset the LRU list to empty |
| `void evict()` | Manual eviction pass — drops everything currently expired under the TTL |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/collection/Cache.cajeta`](../../../runtime/src/cajeta/collection/Cache.cajeta)
- [HashMap](HashMap.md) — the backing key index
- [Optional](../lang/Optional.md) — `get`'s hit/miss result
- [Duration](../time/Duration.md) — the TTL argument
