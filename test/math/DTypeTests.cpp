//
// DTypeTests — tensor-plan.md Phase 1: the dtype descriptor, the dtype-bound
// hierarchy (Numeric ⊃ Floating/Integral/Complex; bool standalone), the NEP-50
// type-based promotion table, and the type→DType bridge DType.of<T>() (folded by
// the codeOf<T> intercept). cajeta.math is lazily parsed; importing
// cajeta.math.DType triggers it.
//
// Factory names are the short forms (i32/f32/u8/…) — the full primitive names
// (int32/float32/…) are reserved keywords and can't name a method. Note the
// boolean type keyword is `boolean` (not `bool`).
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE = "package test;\nimport cajeta.math.DType;\n";

} // namespace

// 1a — descriptors carry the right kind/bits/signedness, and the bound
// hierarchy classifies correctly (the bounds Tensor<? extends …> binds on).
TEST(DTypeTests, dtypeDescriptors) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        #DType f32 = DType.f32();\n"
        "        if (!f32.isFloating()) { return -1; }\n"
        "        if (!f32.isNumeric()) { return -2; }\n"
        "        if (f32.isIntegral()) { return -3; }\n"
        "        if (!f32.isSigned()) { return -4; }\n"
        "        if (f32.bits() != 32) { return -5; }\n"
        "        if (f32.bytes() != 4) { return -6; }\n"
        "        #DType i32 = DType.i32();\n"
        "        if (!i32.isIntegral()) { return -7; }\n"
        "        if (!i32.isSigned()) { return -8; }\n"
        "        if (!i32.isSignedInt()) { return -9; }\n"
        "        if (i32.isFloating()) { return -10; }\n"
        "        if (!i32.isNumeric()) { return -11; }\n"
        "        #DType u32 = DType.u32();\n"
        "        if (!u32.isIntegral()) { return -12; }\n"
        "        if (u32.isSigned()) { return -13; }\n"
        "        if (!u32.isUnsignedInt()) { return -14; }\n"
        "        #DType b = DType.boolType();\n"
        "        if (!b.isBool()) { return -15; }\n"
        "        if (b.isNumeric()) { return -16; }\n"
        "        if (b.isIntegral()) { return -17; }\n"
        "        #DType bf = DType.bf16();\n"
        "        if (!bf.isFloating()) { return -18; }\n"
        "        if (bf.bits() != 16) { return -19; }\n"
        "        #DType f8 = DType.f8e4m3();\n"
        "        if (!f8.isFloating()) { return -20; }\n"
        "        if (f8.bits() != 8) { return -21; }\n"
        "        if (f8.bytes() != 1) { return -22; }\n"
        "        #DType f4 = DType.f4e2m1();\n"
        "        if (f4.bits() != 4) { return -23; }\n"
        "        if (f4.bytes() != 1) { return -24; }\n"
        "        #DType f16 = DType.f16();\n"
        "        if (f16.equals(bf)) { return -25; }\n"   // same width, distinct dtype
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// The codeOf<T> intercept folds T's reified dtype to a constant i32 packing
// (kind << 16) | (bits << 4) | variant.
TEST(DTypeTests, codeOfFoldsConstant) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return DType.codeOf<int32>();\n"   // (1<<16)|(32<<4)|0 = 66048
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 66048);
}

// 1a (bridge) — DType.of<T>() recovers a dtype descriptor from a static type
// parameter, incl. the float variant discrimination (f8e4m3 → variant 2) and
// the standalone boolean dtype; round-trips against the named factories.
TEST(DTypeTests, ofRecoversDtypeFromTypeParam) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        #DType a = DType.of<int32>();\n"
        "        if (!(a.kind() == 1 && a.bits() == 32 && a.variant() == 0)) { return -1; }\n"
        "        #DType b = DType.of<float32>();\n"
        "        if (!(b.kind() == 3 && b.bits() == 32 && b.variant() == 0)) { return -2; }\n"
        "        #DType c = DType.of<uint8>();\n"
        "        if (!(c.kind() == 2 && c.bits() == 8)) { return -3; }\n"
        "        #DType d = DType.of<float64>();\n"
        "        if (!(d.kind() == 3 && d.bits() == 64)) { return -4; }\n"
        "        #DType e = DType.of<int8>();\n"
        "        if (!(e.kind() == 1 && e.bits() == 8)) { return -5; }\n"
        "        #DType f = DType.of<float8e4m3>();\n"
        "        if (!(f.kind() == 3 && f.bits() == 8 && f.variant() == 2)) { return -6; }\n"
        "        #DType g = DType.of<boolean>();\n"
        "        if (g.kind() != 0) { return -7; }\n"
        "        if (g.isNumeric()) { return -8; }\n"
        "        if (!DType.of<float32>().equals(DType.f32())) { return -9; }\n"
        "        if (!DType.of<int64>().equals(DType.i64())) { return -10; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1b — the binary promotion table matches numpy's NEP-50 (type-based) result
// dtype across the standard cross-product. kind codes: INT=1, UINT=2, FLOAT=3.
TEST(DTypeTests, promotionMatchesNep50) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        #DType r1 = DType.promote(DType.i32(), DType.i64());\n"
        "        if (!(r1.kind() == 1 && r1.bits() == 64)) { return -1; }\n"        // int64
        "        #DType r2 = DType.promote(DType.i8(), DType.u8());\n"
        "        if (!(r2.kind() == 1 && r2.bits() == 16)) { return -2; }\n"        // int16
        "        #DType r3 = DType.promote(DType.i64(), DType.u64());\n"
        "        if (!(r3.kind() == 3 && r3.bits() == 64)) { return -3; }\n"        // float64
        "        #DType r4 = DType.promote(DType.i32(), DType.u64());\n"
        "        if (!(r4.kind() == 3 && r4.bits() == 64)) { return -4; }\n"        // float64
        "        #DType r5 = DType.promote(DType.i32(), DType.u8());\n"
        "        if (!(r5.kind() == 1 && r5.bits() == 32)) { return -5; }\n"        // int32
        "        #DType r6 = DType.promote(DType.f32(), DType.i64());\n"
        "        if (!(r6.kind() == 3 && r6.bits() == 64)) { return -6; }\n"        // float64 (numpy/NEP-50: int64 needs >float32)
        "        #DType r7 = DType.promote(DType.f16(), DType.i64());\n"
        "        if (!(r7.kind() == 3 && r7.bits() == 64)) { return -7; }\n"        // float64 (int64 ⊕ float16)
        "        #DType r8 = DType.promote(DType.f32(), DType.f64());\n"
        "        if (!(r8.kind() == 3 && r8.bits() == 64)) { return -8; }\n"        // float64
        "        #DType r9 = DType.promote(DType.boolType(), DType.i8());\n"
        "        if (!(r9.kind() == 1 && r9.bits() == 8)) { return -9; }\n"         // int8
        "        #DType r10 = DType.promote(DType.boolType(), DType.f32());\n"
        "        if (!(r10.kind() == 3 && r10.bits() == 32)) { return -10; }\n"     // float32
        "        #DType r11 = DType.promote(DType.u8(), DType.u32());\n"
        "        if (!(r11.kind() == 2 && r11.bits() == 32)) { return -11; }\n"     // uint32
        "        #DType r12 = DType.promote(DType.f16(), DType.f32());\n"
        "        if (!(r12.kind() == 3 && r12.bits() == 32)) { return -12; }\n"     // float32
        "        #DType r13 = DType.promote(DType.i16(), DType.u16());\n"
        "        if (!(r13.kind() == 1 && r13.bits() == 32)) { return -13; }\n"     // int32
        "        #DType r14 = DType.promote(DType.i16(), DType.u8());\n"
        "        if (!(r14.kind() == 1 && r14.bits() == 16)) { return -14; }\n"     // int16
        "        #DType r15 = DType.promote(DType.f32(), DType.f32());\n"
        "        if (!(r15.kind() == 3 && r15.bits() == 32)) { return -15; }\n"     // float32
        "        #DType r16 = DType.promote(DType.u8(), DType.i8());\n"
        "        if (!(r16.kind() == 1 && r16.bits() == 16)) { return -16; }\n"     // int16 (commutative)
        "        #DType r17 = DType.promote(DType.i32(), DType.f32());\n"
        "        if (!(r17.kind() == 3 && r17.bits() == 64)) { return -17; }\n"     // float64 (int32 needs >float32)
        "        #DType r18 = DType.promote(DType.i16(), DType.f32());\n"
        "        if (!(r18.kind() == 3 && r18.bits() == 32)) { return -18; }\n"     // float32 (int16 fits float32)
        "        #DType r19 = DType.promote(DType.i8(), DType.f16());\n"
        "        if (!(r19.kind() == 3 && r19.bits() == 16)) { return -19; }\n"     // float16 (int8 fits float16)
        "        #DType r20 = DType.promote(DType.i64(), DType.f64());\n"
        "        if (!(r20.kind() == 3 && r20.bits() == 64)) { return -20; }\n"     // float64
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
