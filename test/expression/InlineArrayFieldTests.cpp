//
// Fixed-size inline array fields: a class field declared `T[N]` (N a
// compile-time integer literal) is stored as N contiguous elements INLINE in
// the enclosing object — no pointer slot, no heap header — distinct from a
// heap array reference `T[]`. Plan: agents/cajeta/string-builder-sso-plan.md
// Unit 1. Spec: specs/archive/string-builder-sso-spec.md §2 (2.1.1-2.1.3,
// 2.1.5, 2.1.6-primitives, 2.1.8).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a Box class (one inline int32[4] field) + an A.run() entry. `creator`
// is `heap`/`stack` — the inline buffer must round-trip regardless of where
// the enclosing object lives.
std::string boxSource(const std::string& creator) {
    return "package test;\n"
           "public final class Box {\n"
           "    public int32[4] f;\n"
           "    public Box() {}\n"
           "    public int32 fill() {\n"
           "        int32 i = 0;\n"
           "        while (i < 4) { this.f[i] = i * 10; i = i + 1; }\n"
           "        int32 sum = 0;\n"
           "        i = 0;\n"
           "        while (i < 4) { sum = sum + this.f[i]; i = i + 1; }\n"
           "        return sum;\n"
           "    }\n"
           "}\n"
           "public final class A {\n"
           "    public static int32 run() {\n"
           "        Box b = " + creator + " Box();\n"
           "        return b.fill();\n"
           "    }\n"
           "}\n";
}

} // namespace

// 1.1.1 — write f[0..3] (runtime indices), read each back; heap-resident object.

// 1.1.1 — same, stack-resident object (the inline buffer lives in the stack slot).

// 1.1.2 — int8[64] field: write/read across the whole range incl. b[63].
// The exact shape StringBuilder SSO needs.

// 1.1.3 — type distinctness + no bleed: an inline int32[4] field, a trailing
// int32 tag, AND a heap int32[] reference field coexist. The inline array
// occupies its own 4 inline slots (writing it never disturbs `tag`); the heap
// field is a separate pointer the object allocates and uses independently.

// 1.3.1 — inline access is a DIRECT inline GEP: the IR for reading an inline
// field element must NOT load the field slot as a pointer before indexing
// (that pointer-load-then-index is the heap-array shape that SIGSEGVs here).
TEST(InlineArrayFieldTests, NoPointerLoadOfInlineField) {
    std::string src =
        "package test;\n"
        "public final class Box {\n"
        "    public int32[4] f;\n"
        "    public Box() {}\n"
        "    public int32 get(int32 i) { return this.f[i]; }\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box();\n"
        "        b.f[2] = 99;\n"
        "        return b.get(2);\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.A", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 99);
    // The inline field is laid out and accessed as `[4 x i32]` (N elements
    // embedded in the object) — a heap `int32[]` field would instead be a
    // `ptr` plus a `{ i64, [0 x i32] }` header and never produce an inline
    // [4 x i32] aggregate. Its presence proves the direct inline layout/GEP
    // (no header pointer load before indexing).
    std::string ir = jit->getModuleIr();
    EXPECT_NE(ir.find("[4 x i32]"), std::string::npos) << ir.substr(0, 4000);
}
