// title-stores §3.4 — an array whose ELEMENT TYPE is itself an array must
// be allocated with the tail slot-bitmap, including when the element type
// arrives through a TYPE PARAMETER.
//
// The defect (threaded-forward-path plan 8.13): two adjacent checks in
// CreatorRest disagreed. The per-level pick asks for the bits allocator
// when either
//
//     arrayElementCarriesSlotBits(elem)        // false for arrays
//  || arrayElementCarriesArraySlotBits(elem)   // TRUE for arrays
//
// but the guard deciding whether to FETCH that allocator asked only
//
//     arrayElementCarriesSlotBits(elem) || totalBracketPairs > 1
//
// A literal `int8[16][]` has two bracket pairs and was covered. A generic
// `heap T[cap]` with `T = int8[]` has ONE, so the allocator was never
// fetched, `levelHasBits` short-circuited on the null, and the header came
// back with no tail bitmap — while `data[i] #= v` and the drop walk both
// use one. The slot-bit write then lands past the payload.
//
// It reached us through `ArrayList<int8[]>`: eleven owned arrays added to
// a list, every add succeeding and every element reading back correctly,
// then SIGSEGV in `__libc_free` on the way out. Ten was clean; 1 MB x 16
// was clean; 9.4 MB x 16 was not — heap-LAYOUT dependent, which is why the
// tests below assert the DECISION (which allocator is emitted) and not the
// crash. A corruption test that happens to survive its own layout is a
// test that passes for the wrong reason.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// A container with a `T[] data` field, filled through `#=` — ArrayList's
// shape, reduced to the part that matters.
const char* kGenericBox =
    "package test;\n"
    "public class Box<T> {\n"
    "    public T[] data;\n"
    "    public int32 n;\n"
    "    public Box(int32 cap) {\n"
    "        this.data = heap T[cap];\n"
    "        this.n = 0;\n"
    "    }\n"
    "    public void add(T v) {\n"
    "        this.data[this.n] #= v;\n"
    "        this.n = this.n + 1;\n"
    "    }\n"
    "}\n";

}  // namespace

// The mechanism: `heap T[cap]` with T bound to an ARRAY must reach the
// tail-bitmap allocator. This is the assertion the fix is actually about.
TEST(GenericArrayElementSlotBits, genericArrayElementReachesTheBitsAllocator) {
    const std::string src = std::string(kGenericBox) +
        "public class D {\n"
        "    public static int32 run() {\n"
        "        Box<int8[]> b #= heap Box<int8[]>(16);\n"
        "        int32 i = 0;\n"
        "        while (i < 12) {\n"
        "            int8[] a #= heap int8[64];\n"
        "            a[0] = (int8) i;\n"
        "            b.add(#a);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return (int32) b.data[11][0];\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    const std::string& ir = jit->getModuleIr();
    // An empty capture would make every EXPECT below vacuous — it would
    // "fail" on a missing haystack and "pass" the negative test for the
    // same reason. Assert the instrument before trusting it.
    ASSERT_FALSE(ir.empty()) << "captureIr produced no IR";
    EXPECT_NE(ir.find("__cajeta_new_array_header_bits"), std::string::npos)
        << "a `heap T[cap]` whose T is an array was allocated WITHOUT the "
           "tail slot-bitmap, but `data[i] #= v` writes slot bits into one "
           "— the write lands past the payload. IR:\n"
        << ir.substr(0, 4000);
}

// The does-NOT-fire half. A primitive element carries no slot bits, and a
// fix that reached for the bits allocator unconditionally would make every
// `int32[]` in the language pay for a bitmap nothing reads.
//
// Counted against a BASELINE rather than grepped for absence: the module
// carries the stdlib, which uses the bits allocator legitimately, so
// "`__cajeta_new_array_header_bits` appears nowhere" is false for a correct
// compiler and the first version of this test failed for that reason.
// What is actually claimed is that the user's `int32[]` adds no call.
TEST(GenericArrayElementSlotBits, primitiveElementAddsNoBitsAllocation) {
    const char* kNoArray =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() { return 7; }\n"
        "}\n";
    const char* kPrimArray =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        int32[] a #= heap int32[16];\n"
        "        a[3] = 7;\n"
        "        return a[3];\n"
        "    }\n"
        "}\n";
    auto count = [](const std::string& hay) {
        size_t n = 0, at = 0;
        const std::string needle = "__cajeta_new_array_header_bits";
        while ((at = hay.find(needle, at)) != std::string::npos) { ++n; at += needle.size(); }
        return n;
    };
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto base = CajetaJit::compile(kNoArray, "test.D", opts);
    ASSERT_FALSE(base->getModuleIr().empty()) << "captureIr produced no IR";
    auto prim = CajetaJit::compile(kPrimArray, "test.D", opts);
    ASSERT_FALSE(prim->getModuleIr().empty()) << "captureIr produced no IR";

    EXPECT_EQ(count(prim->getModuleIr()), count(base->getModuleIr()))
        << "a primitive-element array pulled in the tail-bitmap allocator; "
           "it needs no slot bits and must not pay for them";
    auto fn = prim->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Behaviour, alongside the mechanism: every element must survive the round
// trip and the program must complete its teardown.
TEST(GenericArrayElementSlotBits, elevenOwnedArraysSurviveStoreAndTeardown) {
    const std::string src = std::string(kGenericBox) +
        "public class D {\n"
        "    public static int32 run() {\n"
        "        Box<int8[]> b #= heap Box<int8[]>(16);\n"
        "        int32 i = 0;\n"
        "        while (i < 12) {\n"
        "            int8[] a #= heap int8[64];\n"
        "            a[0] = (int8) (i * 3);\n"
        "            b.add(#a);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int32 sum = 0;\n"
        "        i = 0;\n"
        "        while (i < 12) {\n"
        "            sum = sum + (int32) b.data[i][0];\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    // 3 * (0 + 1 + ... + 11) = 3 * 66 = 198
    EXPECT_EQ(fn(), 198);
}

// The literal two-bracket form was ALREADY covered by `totalBracketPairs >
// 1`. Kept so a future simplification of that condition cannot silently
// drop it.
TEST(GenericArrayElementSlotBits, literalNestedArrayStillReachesTheBitsAllocator) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        int8[][] a #= heap int8[4][];\n"
        "        a[0] #= heap int8[8];\n"
        "        a[0][0] = 5;\n"
        "        return (int32) a[0][0];\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    const std::string& ir = jit->getModuleIr();
    // An empty capture would make every EXPECT below vacuous — it would
    // "fail" on a missing haystack and "pass" the negative test for the
    // same reason. Assert the instrument before trusting it.
    ASSERT_FALSE(ir.empty()) << "captureIr produced no IR";
    EXPECT_NE(ir.find("__cajeta_new_array_header_bits"), std::string::npos)
        << "the literal nested-array form lost its tail bitmap. IR:\n"
        << ir.substr(0, 4000);
}
