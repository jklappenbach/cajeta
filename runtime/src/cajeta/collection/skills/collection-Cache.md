---
id: collection-Cache
applies-to: [cajeta/collection/Cache]
title: Cache<K,V> — bounded LRU cache with optional TTL
description: How to construct, read, and maintain cajeta's Cache<K,V> — Optional get(), TTL via setMaxAge(Duration), and the put/remove/evict lifecycle.
---

# Cache<K, V>

A bounded in-memory LRU cache (in `cajeta.collection`, component-paired with its
`CacheNode<K,V>` entry type). Reach for it when you want "compute once, reuse N times,
drop the oldest under pressure" with an optional time-to-live. **This is an access
point** — you construct it directly.

For unbounded key→value lookup, use `HashMap` (see `collection-hash-component`); `Cache`
is built *on* a `HashMap` plus a doubly-linked LRU list, and it adds the bound + TTL.

## Construct with the LRU bound; arm TTL separately

`Cache(int32 maxEntries)` — `maxEntries` must be positive (a 0-bound cache evicts on
every insert). The backing `HashMap` is heap-allocated internally; you only pass the
bound. TTL is **off by default** — enable it with `setMaxAge(Duration)`; pass
`Duration.ofNanos(0)` to disable. Changing the age later only affects subsequent
expiration checks; nothing is eagerly evicted.

```cajeta
import cajeta.collection.Cache;
import cajeta.lang.Optional;
import cajeta.time.Duration;

Cache<int64, String> cache = stack Cache<int64, String>(1024);
cache.setMaxAge(Duration.ofSeconds(30));   // optional TTL

cache.put(42, "answer");

Optional<String> hit = cache.get(42);
String value = hit.isPresent() ? hit.get() : compute(42);
```

## The methods that matter

- **`Optional<V> get(K key)`** — returns `stack Optional<V>`, **empty when the key is
  absent OR expired**. TTL is checked here: an expired entry is *removed as a
  side-effect* and you get empty back. On a hit it bumps access time and moves the entry
  to the MRU head. Guard with `isPresent()` before `get()` on the Optional —
  `Optional.get()` on empty throws (`CAJETA_ERROR_NONE_UNWRAP`), it does not return null.
- **`void put(K key, V value)`** — insert or replace; replacing bumps access time + moves
  to head. Inserting past `maxEntries` evicts the LRU tail immediately.
- **`void remove(K key)`** — unlink the key's entry and free its node; a key or value you
  *lent* is left to its owner, while a value you *transferred* is freed with the entry.
  No-op if absent.
- **`void evict()`** — manual TTL sweep: drops every currently-expired entry. **No-op
  when TTL is disabled.** The size cap is already enforced on every `put`, so `evict` only
  matters for releasing stale entries from an idle cache (call it from a timer/heartbeat).
- **`boolean containsKey(K key)`** — present AND not expired; unlike `get` it does **not**
  touch access time, does **not** move the entry, and leaves an expired entry physically
  in place (next `get`/`evict` reclaims it).
- **`int32 count()`**, **`boolean isEmpty()`**, **`void clear()`** — clear rebuilds the
  backing table and empties the LRU list.

## Ownership & values crossing the boundary

`put(K key, V value)` declares plain formals and forwards both with the transfer word
(`heap CacheNode<K, V>(#key, #value, now)`); `CacheNode` stores them with `#=`. It is the
§2.3 sink model, so the mode you tender at the call site is the mode the entry records:

- `cache.put(k, v)` **lends** — the cache stores your instances and never frees them, so
  both key and value must outlive the entry on your own account, and **nothing diagnoses
  it today** if they do not.
- `cache.put(id, #session)` **transfers** — the node takes title to the value, nothing is
  freed at `put`'s return, and the entry's teardown (`remove`, `evict`, `clear`, or the
  cache's own drop) releases it.

Use the transfer form for anything built fresh per call, which would otherwise dangle. The
`DnsCache.resolve` sites that spell `this.store.put(#key, #DnsCacheEntry.positive(#resolved))`
are the intended pattern for exactly that shape.

One asymmetry worth knowing: the key reaches two slots. The node's key slot records
whichever mode you tendered, while the backing map's key slot is handed `key` plainly
(`this.entries.put(key, #fresh)`) and always borrows — aliasing the node's copy. The node
is the map's value, so the two are removed together and the aliasing is sound.

`get` returns a `stack Optional<V>` holding the stored `V` (you receive the value out,
not the node). Entry `CacheNode`s are heap-allocated and owned by the cache; you never
see them.

## Lifecycle & concurrency

Plain heap/stack object — drops with normal scope/ownership rules; there is **no**
`close()` and nothing to free manually. **Not thread-safe** (same posture as `HashMap`):
wrap shared instances in `Mutex.withLock`. TTL expiration is lazy + manual, not
background — there is no timer thread; expiry happens only on `get`/`containsKey` lookups
and on explicit `evict()` calls.

## The backing-HashMap null trap (do not "fix" the source)

Internally, `Cache` calls `HashMap.get`, which returns the **raw `V` (the node pointer),
returning `null` on a miss — NOT an `Optional`**. The implementation therefore
null-checks the node (`if (node == null)`) rather than calling `isPresent()`/`isEmpty()`.
Treating that raw value as an `Optional` would dispatch `Optional.isEmpty()` on a
possibly-null pointer. This asymmetry is intentional: the **public** `Cache.get` returns
`Optional<V>`; only the private `HashMap` lookups deal in null. See
`collection-hash-component` for the HashMap miss-path contract.
