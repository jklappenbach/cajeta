---
id: io-net-dns-overview
applies-to: [cajeta/io/net/dns]
title: DNS package — blocking host resolution + TTL cache
description: Resolve a hostname to SocketAddress[] via Dns.resolve, memoize with DnsCache over a swappable Resolver; ownership, families, and DNS error types.
---

# cajeta.io.net.dns — name resolution

Turn a hostname (or numeric IP literal) into the `SocketAddress[]` it maps
to. Two access points:

| Want to | Use |
| --- | --- |
| Resolve a name once, blocking | `Dns.resolve(host[, port][, family])` (static) |
| Resolve with memoization (LRU + TTL + negative caching) | `DnsCache` (instance; wraps a `Resolver`) |
| Restrict to v4/v6 | pass a `ResolveFamily` (`V4_ONLY` / `V6_ONLY` / `BOTH`) |
| Inject a fake backend in a test | `DnsCache.withResolver(#Resolver)` |
| Branch on "name not found" vs "lookup failed" | catch `UnknownHostException` vs `ResolutionFailedException` |

What this package does **not** do: no async resolve (`Dns.resolveAsync`
is a later plan item, not present); no reverse DNS / PTR; no SRV/MX
records — `getaddrinfo` A/AAAA only. `DnsCache` cannot honor the DNS
record TTL (`getaddrinfo` hides it) — it applies one configurable default
TTL instead.

## Inventory

**Entry points (instantiate/call):**
- `Dns` — all-static, no instances. The blocking `getaddrinfo` primitive
  every layer builds on.
- `DnsCache` — bounded LRU keyed on `(host, family)` over
  `cajeta.collection.Cache<K,V>`, with negative-result caching.
- `SystemResolver` — the production `Resolver`; a thin adapter onto
  `Dns.resolve(host, 0, family)`. Stateless; one shared instance suffices.

**Support types (do not instantiate as call targets):**
- `Resolver` — the miss-backend interface (`#SocketAddress[] resolve(String, ResolveFamily)`).
  The test seam.
- `ResolveFamily` — enum: `V4_ONLY`, `V6_ONLY`, `BOTH`.
- `DnsCacheEntry` — internal cache value (positive array | negative flag); you do not touch it directly.
- `ResolveErrors` — the `cajeta_resolve_err` ordinal table + `fromResolveErrno`.
- `UnknownHostException` / `ResolutionFailedException` — both extend `cajeta.io.net.NetException`.

## Collaboration

`DnsCache` owns LRU+TTL bookkeeping; the `Resolver` owns *how a name
becomes addresses*. On a **miss**, `DnsCache.resolve` calls
`resolver.resolve(host, family)` (production: `SystemResolver` →
`Dns.resolve` → `getaddrinfo`); on a **hit inside the TTL** it never calls
through. `Dns.resolve`'s failure path classifies the thread-local resolve
ordinal: `NONAME`/`NODATA` → `UnknownHostException`, everything else →
`ResolutionFailedException` (the same split `ResolveErrors.fromResolveErrno`
tabulates). `SystemResolver` propagates those unchanged.

## Ownership & lifecycle (the load-bearing part)

- Every `resolve(...)` returns a **freshly owned** `#SocketAddress[]`
  (always non-empty — a zero-address lookup throws instead). The caller
  owns it.
- `DnsCache.resolve` returns a **fresh port-baked copy on every call**,
  even on a cache hit: the cache stores **port-0** addresses keyed on
  `(host, family)` and re-bakes your port per result. Never mutate a
  returned array under the assumption it is private to you on a miss — it
  is, but the cached canonical array is separate and port-0.
- `DnsCache(maxEntries, ttl, #resolver)` and `withResolver(#resolver)`
  **take ownership** of the resolver. `DnsCacheEntry.positive(#addrs)`
  takes ownership of the array.
- `Dns` is static — nothing to construct, nothing to close. `DnsCache`
  needs no `close()`; it holds only the `Cache` and resolver.
- The native `getaddrinfo` result block is released inside `Dns.resolve`
  (NULL-safe) before return — nothing the caller gets aliases it.

## Invariants & hazards

- `DnsCache` is **not thread-safe** (like the `Cache` it wraps) — guard a
  shared instance with a `Mutex`.
- `ResolveFamily` ordinals are the portable enum positions
  (`V4_ONLY=0, V6_ONLY=1, BOTH=2`) — **not** the native filter values.
  `Dns` maps them internally; do not pass the enum ordinal as a native
  filter.
- `(host, family)` is the cache key; **port is not part of it**. `host:80`
  and `host:443` share one entry.
- DNS exceptions set inherited `kind = KIND_OTHER (99)`; the precise cause
  is on `resolveErrno` (e.g. retry policies can single out `AGAIN`).
- Negative caching is on by default; `setCacheFailures(false)` makes every
  failure re-hit the backend.

## Example

```cajeta
import cajeta.io.net.SocketAddress;
import cajeta.io.net.dns.Dns;
import cajeta.io.net.dns.DnsCache;
import cajeta.io.net.dns.ResolveFamily;
import cajeta.io.net.dns.UnknownHostException;

// One-shot blocking lookup (owned, non-empty).
SocketAddress[] addrs #= Dns.resolve("example.test", 443, ResolveFamily.BOTH);

// Memoized: the second call inside the TTL returns a fresh port-baked
// copy without a second getaddrinfo.
DnsCache cache #= DnsCache.create();                 // default size + TTL + SystemResolver
SocketAddress[] a #= cache.resolve("example.test", 443, ResolveFamily.BOTH);
SocketAddress[] b #= cache.resolve("example.test", 443, ResolveFamily.BOTH);

try {
    SocketAddress[] dead #= Dns.resolve("no.such.host.test", 80);
} catch (UnknownHostException e) {
    // name definitively does not resolve (NONAME/NODATA); e.resolveErrno has the ordinal
}
```

Test injecting a fake backend (mirrors `DnsCacheTests`):

```cajeta
// A Resolver fake; DnsCache.withResolver takes ownership of it.
DnsCache cache #= DnsCache.withResolver(heap CountingResolver());
```

## Down a level

`Dns` (signatures, `getaddrinfo` bridge), `DnsCache` (TTL/LRU/negative
detail), `Resolver`/`SystemResolver` (the seam), and the `ResolveErrors`
ordinal table each carry their own per-symbol detail.
