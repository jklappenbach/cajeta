//
// Tests for the cajeta.hash runtime primitives. The test binary links
// the C runtime natively (see src/CMakeLists.txt — CAJETA_RT_SRC is
// added to the test target as well as compiled to bitcode for JIT),
// so the test calls __cajeta_hash_* functions directly via extern "C".
//
// Two layers of coverage:
//   - Determinism: same input within the same process produces the
//     same hash output across repeated calls.
//   - Distribution sanity: different inputs (small deltas) produce
//     very different outputs — the mixer must avalanche.
//   - Seed behavior: the per-process seed is stable within a run, so
//     pinning specific output values is brittle; tests verify
//     properties (avalanche, identity equality) instead.
//

#include "gtest/gtest.h"

#include <cstdint>
#include <cstring>
#include <unordered_set>

extern "C" {
    int64_t __cajeta_hash_seed(void);
    int64_t __cajeta_hash_int64(int64_t value);
    int64_t __cajeta_hash_int32(int32_t value);
    int64_t __cajeta_hash_float64(double value);
    int64_t __cajeta_hash_float32(float value);
    int64_t __cajeta_hash_boolean(int8_t value);
    int64_t __cajeta_hash_identity(void* p);
    int64_t __cajeta_hash_combine(int64_t a, int64_t b);
}

// --- seed ------------------------------------------------------------------

// The per-process seed should be non-zero and stable across reads
// within the same process. The constructor function fires before
// main() so the value is already initialized by the time gtest runs.
TEST(HashSeedTests, seedInitializedAndStable) {
    int64_t s1 = __cajeta_hash_seed();
    int64_t s2 = __cajeta_hash_seed();
    EXPECT_NE(s1, 0);
    EXPECT_EQ(s1, s2);
}

// --- int64 -----------------------------------------------------------------

TEST(HashInt64Tests, deterministic) {
    EXPECT_EQ(__cajeta_hash_int64(42), __cajeta_hash_int64(42));
    EXPECT_EQ(__cajeta_hash_int64(0), __cajeta_hash_int64(0));
    EXPECT_EQ(__cajeta_hash_int64(-1), __cajeta_hash_int64(-1));
}

TEST(HashInt64Tests, distinguishesNeighbors) {
    // The mixer must avalanche — a one-bit input change should produce
    // a wildly different output, not just flip one bit of the result.
    int64_t h0 = __cajeta_hash_int64(0);
    int64_t h1 = __cajeta_hash_int64(1);
    EXPECT_NE(h0, h1);

    // Count differing bits — expect roughly half (32 of 64) flipped.
    // Be loose: anywhere between 10 and 54 differing bits is fine; a
    // pathological mixer would flip 0-1 or all 64.
    int diff = __builtin_popcountll((uint64_t) h0 ^ (uint64_t) h1);
    EXPECT_GE(diff, 10);
    EXPECT_LE(diff, 54);
}

TEST(HashInt64Tests, distinctValuesProduceDistinctHashesUsually) {
    // 1024 distinct inputs, hash them all, expect 0 or extremely few
    // collisions. A well-mixed 64-bit hash should see ~0 collisions
    // for any reasonable sample size; the birthday bound is at ~2^32.
    std::unordered_set<int64_t> seen;
    for (int64_t i = 0; i < 1024; ++i) {
        seen.insert(__cajeta_hash_int64(i));
    }
    EXPECT_EQ(seen.size(), 1024u);
}

// --- int32 -----------------------------------------------------------------

TEST(HashInt32Tests, deterministic) {
    EXPECT_EQ(__cajeta_hash_int32(42), __cajeta_hash_int32(42));
    EXPECT_EQ(__cajeta_hash_int32(0), __cajeta_hash_int32(0));
    EXPECT_EQ(__cajeta_hash_int32(INT32_MIN), __cajeta_hash_int32(INT32_MIN));
}

TEST(HashInt32Tests, distinguishesFromInt64SameLowBits) {
    // (int32) -1 and (int64) -1 share the same all-ones bit pattern in
    // the low 32 bits. After sign-extension to int64, they should still
    // hash identically (because they're the same conceptual value), so
    // verify the sign-extension is happening correctly.
    EXPECT_EQ(__cajeta_hash_int32(-1), __cajeta_hash_int64(-1));
    EXPECT_EQ(__cajeta_hash_int32(0), __cajeta_hash_int64(0));
    EXPECT_EQ(__cajeta_hash_int32(42), __cajeta_hash_int64(42));
}

// --- float64 ---------------------------------------------------------------

TEST(HashFloat64Tests, deterministic) {
    EXPECT_EQ(__cajeta_hash_float64(3.14), __cajeta_hash_float64(3.14));
    EXPECT_EQ(__cajeta_hash_float64(0.0), __cajeta_hash_float64(0.0));
}

TEST(HashFloat64Tests, canonicalizesNegativeZero) {
    // IEEE-754: +0.0 == -0.0. They must hash identically per the
    // hash/equals contract.
    EXPECT_EQ(__cajeta_hash_float64(0.0), __cajeta_hash_float64(-0.0));
}

TEST(HashFloat64Tests, distinguishesNeighbors) {
    EXPECT_NE(__cajeta_hash_float64(0.0), __cajeta_hash_float64(1.0));
    EXPECT_NE(__cajeta_hash_float64(1.0), __cajeta_hash_float64(1.0000001));
}

// --- float32 ---------------------------------------------------------------

TEST(HashFloat32Tests, deterministic) {
    EXPECT_EQ(__cajeta_hash_float32(3.14f), __cajeta_hash_float32(3.14f));
}

TEST(HashFloat32Tests, canonicalizesNegativeZero) {
    EXPECT_EQ(__cajeta_hash_float32(0.0f), __cajeta_hash_float32(-0.0f));
}

// --- boolean ---------------------------------------------------------------

TEST(HashBooleanTests, twoDistinctValues) {
    int64_t hFalse = __cajeta_hash_boolean(0);
    int64_t hTrue  = __cajeta_hash_boolean(1);
    EXPECT_NE(hFalse, hTrue);
    // Any nonzero int8 maps to "true" (1), so 1, 2, 255, -1 all hash
    // identically to the true-bucket.
    EXPECT_EQ(__cajeta_hash_boolean(1), __cajeta_hash_boolean(2));
    EXPECT_EQ(__cajeta_hash_boolean(1), __cajeta_hash_boolean(-1));
}

// --- identity --------------------------------------------------------------

TEST(HashIdentityTests, deterministic) {
    int dummy;
    void* p = &dummy;
    EXPECT_EQ(__cajeta_hash_identity(p), __cajeta_hash_identity(p));
}

TEST(HashIdentityTests, distinctPointersDistinctHashes) {
    int a, b;
    EXPECT_NE(__cajeta_hash_identity(&a), __cajeta_hash_identity(&b));
}

TEST(HashIdentityTests, nullHashesConsistently) {
    EXPECT_EQ(__cajeta_hash_identity(nullptr), __cajeta_hash_identity(nullptr));
}

// --- @Native annotation bridge ---------------------------------------------
// Validates that cajeta-source @Native methods correctly forward to the C
// runtime. This is the bridge that lets cajeta stdlib classes (cajeta.hash.
// Hash, cajeta.io.Buffer, etc.) live in runtime/src/cajeta/... and route
// to C runtime helpers without compiler-side intrinsic dispatch.
//
// Notes for these tests: the C runtime is compiled twice — once to LLVM
// bitcode (embedded + JIT-linked into user modules) and once natively
// (linked into the test binary for direct C++ calls). Each copy has its
// own static __cajeta_hash_seed_value, seeded independently from
// /dev/urandom on first access. Tests can't expect bit-equal results
// across the cajeta-JIT-path vs C-direct-call boundary; what they can
// expect is that the same call shape produces consistent results within
// each path.

#include "../jit/JitTestHelper.h"
using cajeta_test::CajetaJit;

// @Native on a no-arg static method — the cajeta call resolves the
// runtime symbol and returns a non-zero seed (the bitcode-side seed
// was lazily initialized on this first call).
TEST(NativeAnnotationTests, forwardsNoArgStatic) {
    auto src =
        "package test;\n"
        "public final class Caller {\n"
        "    @Native(\"__cajeta_hash_seed\")\n"
        "    public static int64 seed() { }\n"
        "    public static int64 run() {\n"
        "        return seed();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Caller");
    auto fn = jit->lookup<int64_t (*)()>("run");
    int64_t fromCajeta = fn();
    EXPECT_NE(fromCajeta, 0);
    // Stable across repeated calls within the same JIT instance.
    EXPECT_EQ(fn(), fromCajeta);
}

// @Native on a method that takes parameters and returns a value —
// the wrapper must forward every argument to the runtime symbol.
// Verifies via deterministic+distinct: same input → same output;
// different inputs → different outputs.
TEST(NativeAnnotationTests, forwardsArgumentsAndReturnValue) {
    auto src =
        "package test;\n"
        "public final class Caller {\n"
        "    @Native(\"__cajeta_hash_int64\")\n"
        "    public static int64 hashInt64(int64 value) { }\n"
        "    public static int64 hashOf(int64 v) {\n"
        "        return hashInt64(v);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Caller");
    auto fn = jit->lookup<int64_t (*)(int64_t)>("hashOf");
    int64_t h42 = fn(42);
    int64_t h7  = fn(7);
    int64_t h42_again = fn(42);
    EXPECT_EQ(h42, h42_again);     // deterministic
    EXPECT_NE(h42, h7);            // distinct inputs → distinct hashes
    EXPECT_NE(h42, 0);
}

// Two @Native methods pointing at the same runtime symbol — the wrapper
// extern declaration must dedupe (one runtime function declaration per
// module, not one per @Native call site). Verified by checking that
// both wrappers produce the same value for the same input.
TEST(NativeAnnotationTests, deduplicatesRuntimeSymbolDeclarations) {
    auto src =
        "package test;\n"
        "public final class Caller {\n"
        "    @Native(\"__cajeta_hash_int64\")\n"
        "    public static int64 hash1(int64 v) { }\n"
        "    @Native(\"__cajeta_hash_int64\")\n"
        "    public static int64 hash2(int64 v) { }\n"
        "    public static int64 viaHash1(int64 v) { return hash1(v); }\n"
        "    public static int64 viaHash2(int64 v) { return hash2(v); }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Caller");
    auto fn1 = jit->lookup<int64_t (*)(int64_t)>("viaHash1");
    auto fn2 = jit->lookup<int64_t (*)(int64_t)>("viaHash2");
    EXPECT_EQ(fn1(100), fn2(100));   // both routes resolve to the same runtime fn
    EXPECT_NE(fn1(100), 0);
}

// --- cajeta.hash.Hash stdlib-class round-trip ------------------------------
// First stdlib class using @Native lives at runtime/src/cajeta/hash/Hash.cajeta.
// User code calls Hash.processSeed() / Hash.combine() and the stdlib parse
// picks up the class, the @Native annotations bridge to the runtime symbols.

TEST(CajetaHashClassTests, processSeedFromUserCode) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        return Hash.processSeed();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    int64_t seed = fn();
    EXPECT_NE(seed, 0);
    EXPECT_EQ(fn(), seed);   // stable across calls
}

TEST(CajetaHashClassTests, combineFromUserCode) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        return Hash.combine(1, 2);\n"
        "    }\n"
        "    public static int64 run2() {\n"
        "        return Hash.combine(2, 1);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn1 = jit->lookup<int64_t (*)()>("run");
    auto fn2 = jit->lookup<int64_t (*)()>("run2");
    int64_t v12 = fn1();
    int64_t v21 = fn2();
    EXPECT_NE(v12, 0);
    EXPECT_NE(v12, v21);   // combine is not symmetric — order matters
}

// --- combine ---------------------------------------------------------------

TEST(HashCombineTests, deterministic) {
    EXPECT_EQ(__cajeta_hash_combine(1, 2), __cajeta_hash_combine(1, 2));
}

TEST(HashCombineTests, orderMatters) {
    // combine(a, b) != combine(b, a) — the function is not symmetric,
    // which is what you want for structural hashing where field order
    // is part of identity.
    EXPECT_NE(__cajeta_hash_combine(1, 2), __cajeta_hash_combine(2, 1));
}

TEST(HashCombineTests, distinguishesFromEitherInput) {
    EXPECT_NE(__cajeta_hash_combine(0, 0), 0);
    int64_t a = __cajeta_hash_int64(42);
    int64_t b = __cajeta_hash_int64(7);
    EXPECT_NE(__cajeta_hash_combine(a, b), a);
    EXPECT_NE(__cajeta_hash_combine(a, b), b);
}
