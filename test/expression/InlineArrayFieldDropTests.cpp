//
// Drop safety for fixed-size inline array fields (SSO plan Unit 2; spec 2.1.4,
// 2.1.8). An inline `T[N]` field is storage embedded in the object, NOT a heap
// allocation — the auto-drop body must NOT call __cajeta_free_array on it
// (loading the inline bytes as a pointer and freeing them is an invalid free;
// under strict live-set it is silently skipped, but with live-set off it
// corrupts the heap, and if the inline bytes alias a live pointer it's a UAF).
// Heap `T[]` fields on the same object must still be freed.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

size_t countOccurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) { ++n; pos += needle.size(); }
    return n;
}

} // namespace

// 2.2.1 / 2.3.1 — an object whose ONLY array field is inline emits NO
// __cajeta_free_array anywhere (not even a declaration): nothing to free.
TEST(InlineArrayFieldDropTests, InlineOnlyFieldEmitsNoArrayFree) {
    std::string src =
        "package test;\n"
        "public final class Buf {\n"
        "    public int8[64] inlbuf;\n"
        "    public Buf() {}\n"
        "    public int32 fill() {\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { this.inlbuf[i] = (int8) i; i = i + 1; }\n"
        "        return (int32) this.inlbuf[63];\n"
        "    }\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Buf x = heap Buf();\n"   // dropped at scope exit
        "        return x.fill();\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.A", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 63);
    // The array-free path emits a named `drop_arr_slot_<field>` GEP only for a
    // field it actually frees. The inline field `inlbuf` must never produce one.
    // (Counting bare __cajeta_free_array would match the embedded stdlib, which
    // the whole-module IR includes; `inlbuf` is a unique name that doesn't.)
    std::string ir = jit->getModuleIr();
    EXPECT_EQ(countOccurrences(ir, "drop_arr_slot_inlbuf"), 0u) << ir.substr(0, 2000);
}

// 2.1.2 / 2.3.1 — a mixed object (inline buffer + heap reference): the heap
// field IS freed (one __cajeta_free_array), the inline field is NOT. Without
// the fix the inline field would add a second free of garbage.
TEST(InlineArrayFieldDropTests, MixedFreesHeapNotInline) {
    std::string src =
        "package test;\n"
        "public final class Mix {\n"
        "    public int8[64] inl;\n"
        "    public int8[] heapRef;\n"
        "    public Mix() {}\n"
        "    public int32 run2() {\n"
        "        this.heapRef = heap int8[16];\n"
        "        this.inl[0] = 5;\n"
        "        this.heapRef[0] = 9;\n"
        "        return (int32) this.inl[0] + (int32) this.heapRef[0];\n"
        "    }\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Mix m = heap Mix();\n"
        "        return m.run2();\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.A", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 14);
    // The heap field `heapRef` IS freed (its named drop GEP is emitted); the
    // inline field `inl` is NOT (no drop GEP for it).
    std::string ir = jit->getModuleIr();
    EXPECT_GT(countOccurrences(ir, "drop_arr_slot_heapRef"), 0u) << ir.substr(0, 2000);
    EXPECT_EQ(countOccurrences(ir, "drop_arr_slot_inl"), 0u) << ir.substr(0, 2000);
}

// 2.1.1 — create + drop a class with an inline int8[64] field in a tight loop
// with live-set tracking OFF (the release configuration): the bogus inline
// free, if emitted, would corrupt the heap / crash. With the fix it's a clean
// run. Returns an accumulated checksum so the loop isn't optimized away.
TEST(InlineArrayFieldDropTests, TightCreateDropLoopLiveSetOff) {
    std::string src =
        "package test;\n"
        "public final class Buf {\n"
        "    public int8[64] inlbuf;\n"
        "    public Buf() {}\n"
        "    public int32 stamp(int32 seed) {\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { this.inlbuf[i] = (int8)(seed + i); i = i + 1; }\n"
        "        return (int32) this.inlbuf[7];\n"
        "    }\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        int32 acc = 0;\n"
        "        int32 k = 0;\n"
        "        while (k < 5000) {\n"
        "            Buf x = heap Buf();\n"   // created + dropped each iteration
        "            acc = acc + x.stamp(k);\n"
        "            k = k + 1;\n"
        "        }\n"
        "        return acc & 0xFFFF;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.liveSetMode = cajeta::LiveSet::Off;
    auto jit = CajetaJit::compile(src, "test.A", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // Just assert it completes without crashing/corruption; value is the
    // accumulated b[7] = (k+7) low bits over k in [0,5000).
    volatile int32_t r = fn();
    (void) r;
    SUCCEED();
}
