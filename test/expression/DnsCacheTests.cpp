// Tests for cajeta.io.net.dns.DnsCache + the Resolver seam —
// plan item NET-2.4 (DNS TTL cache, bounded LRU keyed on (host, family),
// with negative-result caching, over cajeta.collection.Cache<K,V>).
//
// Scope: the pure-Cajeta cache logic — cache hit/miss, the injectable
// Resolver backend (the test seam the acceptance names), the
// (host, family) key distinctness, per-call port baking (the port is
// NOT part of the key), negative-result caching + its toggle, the LRU
// bound, and TTL expiry. None of these touch the network: every test
// injects a *counting* Resolver fake and asserts on the call count, so
// the suite is deterministic and offline. The production SystemResolver
// (which drives the real NET-2.2 getaddrinfo path) is exercised
// transitively by DnsTests; here we pin the memoization layer above it.
//
// These pin the NET-2.4 slice of the Phase-2 acceptance:
//   - DnsCacheTests.cacheHitSkipsSecondLookup
//        (plan: "Second resolve of the same host inside the TTL hits
//         the cache (no second getaddrinfo; verified via an injectable
//         resolver counter)" — DnsTests.cacheHitSkipsSecondLookup.)
//   - the supporting properties (clear, family distinctness, port
//     baking, negative caching + toggle, LRU bound, TTL expiry).
//
// Harness: a single compilation unit declaring the entry class `A`
// alongside two Resolver fakes — a CountingResolver (bumps a static
// counter, returns a port-0 loopback) and a FailingResolver (bumps a
// counter, throws). A.run() -> int32 returns 1 on the expected result,
// a negative diagnostic otherwise. Multiple top-level classes in one
// source mirrors HashMapStreamTests' Tag/Acc/D layout; the static
// counter mirrors its `Acc.total` mutable-static pattern.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wrap an entry-class body in a unit that also defines the two Resolver
// fakes. The body must `return` an int32.
std::string withCounting(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.io.net.IpAddress;\n"
           "import cajeta.io.net.SocketAddress;\n"
           "import cajeta.io.net.NetException;\n"
           "import cajeta.time.Duration;\n"
           "import cajeta.io.net.dns.DnsCache;\n"
           "import cajeta.io.net.dns.Resolver;\n"
           "import cajeta.io.net.dns.ResolveFamily;\n"
           // A counting Resolver fake: every resolve() bumps a static
           // counter and returns a single port-0 loopback address.
           "public final class CountingResolver implements Resolver {\n"
           "    public static int32 calls = 0;\n"
           "    public #SocketAddress[] resolve(String host, ResolveFamily family) {\n"
           "        CountingResolver.calls = CountingResolver.calls + 1;\n"
           "        SocketAddress[] out = heap SocketAddress[1];\n"
           "        IpAddress ip #= IpAddress.loopbackV4();\n"
           "        out[0] = SocketAddress.of(#ip, 0);\n"
           "        return #out;\n"
           "    }\n"
           "}\n"
           // A Resolver that always fails — bumps the counter then throws.
           "public final class FailingResolver implements Resolver {\n"
           "    public static int32 calls = 0;\n"
           "    public #SocketAddress[] resolve(String host, ResolveFamily family) {\n"
           "        FailingResolver.calls = FailingResolver.calls + 1;\n"
           "        throw heap NetException(\"boom\", NetException.KIND_OTHER);\n"
           "    }\n"
           "}\n"
           "public final class A {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// ============ a second resolve inside the TTL skips the backend ===========

TEST(DnsCacheTests, cacheHitSkipsSecondLookup) {
    // The headline NET-2.4 acceptance: resolve the same (host, family)
    // twice; the backend resolver is invoked exactly once. The second
    // call is served from the cache.
    EXPECT_EQ(runI32(withCounting(
        "CountingResolver.calls = 0;\n"
        "DnsCache cache #= DnsCache.withResolver(heap CountingResolver());\n"
        "SocketAddress[] a #= cache.resolve(\"example.test\", 443, ResolveFamily.BOTH);\n"
        "SocketAddress[] b #= cache.resolve(\"example.test\", 443, ResolveFamily.BOTH);\n"
        "if (CountingResolver.calls != 1) { return -1; }\n"   // one lookup only
        "if (a.count() != 1 || b.count() != 1) { return -2; }\n"
        "if (!b[0].getIp().isV4()) { return -3; }\n"
        "return 1;")), 1);
}

// ============ the cached addresses are correct on the hit =================

TEST(DnsCacheTests, cachedHitReturnsResolvedAddress) {
    // The value served from the cache is the resolver's address
    // (127.0.0.1), with the caller's port baked on.
    EXPECT_EQ(runI32(withCounting(
        "CountingResolver.calls = 0;\n"
        "DnsCache cache #= DnsCache.withResolver(heap CountingResolver());\n"
        "cache.resolve(\"h.test\", 80, ResolveFamily.BOTH);\n"
        "SocketAddress[] b #= cache.resolve(\"h.test\", 80, ResolveFamily.BOTH);\n"
        "IpAddress ip = b[0].getIp();\n"
        "int8[] o = ip.getOctets();\n"
        "int32 o0 = ((int32) o[0]) & 0xff;\n"
        "int32 o3 = ((int32) o[3]) & 0xff;\n"
        "if (o0 != 127 || o3 != 1) { return -1; }\n"
        "return (b[0].getPort() == 80) ? 1 : 0;")), 1);
}

// ============ clear() forces a re-resolve =================================

TEST(DnsCacheTests, clearForcesReResolve) {
    // After clear(), the next lookup is a miss → the backend is hit
    // again (call count climbs to 2).
    EXPECT_EQ(runI32(withCounting(
        "CountingResolver.calls = 0;\n"
        "DnsCache cache #= DnsCache.withResolver(heap CountingResolver());\n"
        "cache.resolve(\"h.test\", 0, ResolveFamily.BOTH);\n"
        "cache.clear();\n"
        "cache.resolve(\"h.test\", 0, ResolveFamily.BOTH);\n"
        "return (CountingResolver.calls == 2) ? 1 : 0;")), 1);
}

// ============ (host, family) keys are distinct ===========================

TEST(DnsCacheTests, distinctFamiliesCacheSeparately) {
    // Same host, two different families → two distinct keys → two
    // backend lookups; a third call repeating the first family hits the
    // cache (stays at 2).
    EXPECT_EQ(runI32(withCounting(
        "CountingResolver.calls = 0;\n"
        "DnsCache cache #= DnsCache.withResolver(heap CountingResolver());\n"
        "cache.resolve(\"h.test\", 0, ResolveFamily.BOTH);\n"
        "cache.resolve(\"h.test\", 0, ResolveFamily.V4_ONLY);\n"
        "if (CountingResolver.calls != 2) { return -1; }\n"
        "cache.resolve(\"h.test\", 0, ResolveFamily.BOTH);\n"      // hit
        "return (CountingResolver.calls == 2) ? 1 : 0;")), 1);
}

// ============ distinct hosts cache separately ============================

TEST(DnsCacheTests, distinctHostsCacheSeparately) {
    // Two different hosts → two keys → two lookups.
    EXPECT_EQ(runI32(withCounting(
        "CountingResolver.calls = 0;\n"
        "DnsCache cache #= DnsCache.withResolver(heap CountingResolver());\n"
        "cache.resolve(\"a.test\", 0, ResolveFamily.BOTH);\n"
        "cache.resolve(\"b.test\", 0, ResolveFamily.BOTH);\n"
        "return (CountingResolver.calls == 2) ? 1 : 0;")), 1);
}

// ============ the port is baked per call, not part of the key ============

TEST(DnsCacheTests, portIsBakedPerCallNotCached) {
    // Two resolves of the same host with *different ports* both hit the
    // single cached entry (one backend lookup), yet each returns its own
    // requested port — the port is not part of the cache key.
    EXPECT_EQ(runI32(withCounting(
        "CountingResolver.calls = 0;\n"
        "DnsCache cache #= DnsCache.withResolver(heap CountingResolver());\n"
        "SocketAddress[] a #= cache.resolve(\"h.test\", 80, ResolveFamily.BOTH);\n"
        "SocketAddress[] b #= cache.resolve(\"h.test\", 443, ResolveFamily.BOTH);\n"
        "if (CountingResolver.calls != 1) { return -1; }\n"     // one lookup
        "if (a[0].getPort() != 80) { return -2; }\n"
        "if (b[0].getPort() != 443) { return -3; }\n"
        "return 1;")), 1);
}

// ============ a negative result is cached and re-raised ==================

TEST(DnsCacheTests, negativeResultIsCachedAndReRaises) {
    // A failing lookup raises; the failure is cached, so a second
    // attempt re-raises *without* re-invoking the (failing) backend —
    // the failing resolver is called exactly once.
    EXPECT_EQ(runI32(withCounting(
        "FailingResolver.calls = 0;\n"
        "DnsCache cache #= DnsCache.withResolver(heap FailingResolver());\n"
        "int32 raises = 0;\n"
        "try { cache.resolve(\"dead.test\", 0, ResolveFamily.BOTH); }\n"
        "catch (NetException e) { raises = raises + 1; }\n"
        "try { cache.resolve(\"dead.test\", 0, ResolveFamily.BOTH); }\n"
        "catch (NetException e) { raises = raises + 1; }\n"
        "if (raises != 2) { return -1; }\n"                     // both raised
        "return (FailingResolver.calls == 1) ? 1 : 0;")), 1);   // backend once
}

// ============ negative caching can be turned off =========================

TEST(DnsCacheTests, negativeCachingDisabledReResolves) {
    // With setCacheFailures(false), a failure is NOT cached, so the
    // second attempt re-tries the backend (call count climbs to 2).
    EXPECT_EQ(runI32(withCounting(
        "FailingResolver.calls = 0;\n"
        "DnsCache cache #= DnsCache.withResolver(heap FailingResolver());\n"
        "cache.setCacheFailures(false);\n"
        "try { cache.resolve(\"dead.test\", 0, ResolveFamily.BOTH); }\n"
        "catch (NetException e) { }\n"
        "try { cache.resolve(\"dead.test\", 0, ResolveFamily.BOTH); }\n"
        "catch (NetException e) { }\n"
        "return (FailingResolver.calls == 2) ? 1 : 0;")), 1);
}

// ============ the LRU bound evicts the oldest entry ======================

TEST(DnsCacheTests, lruBoundEvictsOldest) {
    // A cache of size 1: resolving a second host evicts the first, so
    // re-resolving the first is a miss → a fourth backend call overall.
    // (1) a.test miss, (2) b.test miss + evicts a.test,
    // (3) a.test miss again → calls == 3.
    EXPECT_EQ(runI32(withCounting(
        "CountingResolver.calls = 0;\n"
        "DnsCache cache = heap DnsCache(1, Duration.ofSeconds(30), heap CountingResolver());\n"
        "cache.resolve(\"a.test\", 0, ResolveFamily.BOTH);\n"
        "cache.resolve(\"b.test\", 0, ResolveFamily.BOTH);\n"
        "cache.resolve(\"a.test\", 0, ResolveFamily.BOTH);\n"   // evicted → miss
        "return (CountingResolver.calls == 3) ? 1 : 0;")), 1);
}

// ============ TTL expiry forces a re-resolve =============================

TEST(DnsCacheTests, ttlExpiryReResolves) {
    // A 1-nanosecond TTL: by the time the second lookup runs, any
    // elapsed wall-clock exceeds the TTL, so the entry is treated as
    // expired and re-resolved (call count climbs to 2). Pins that the
    // DnsCache honors the Cache<K,V> TTL it configures.
    EXPECT_EQ(runI32(withCounting(
        "CountingResolver.calls = 0;\n"
        "DnsCache cache = heap DnsCache(1024, Duration.ofNanos(1), heap CountingResolver());\n"
        "cache.resolve(\"h.test\", 0, ResolveFamily.BOTH);\n"
        "cache.resolve(\"h.test\", 0, ResolveFamily.BOTH);\n"
        "return (CountingResolver.calls == 2) ? 1 : 0;")), 1);
}
