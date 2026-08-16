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
        "        DType a = DType.of<int32>();\n"
        "        if (!(a.kind() == 1 && a.bits() == 32 && a.variant() == 0)) { return -1; }\n"
        "        DType b = DType.of<float32>();\n"
        "        if (!(b.kind() == 3 && b.bits() == 32 && b.variant() == 0)) { return -2; }\n"
        "        DType c = DType.of<uint8>();\n"
        "        if (!(c.kind() == 2 && c.bits() == 8)) { return -3; }\n"
        "        DType d = DType.of<float64>();\n"
        "        if (!(d.kind() == 3 && d.bits() == 64)) { return -4; }\n"
        "        DType e = DType.of<int8>();\n"
        "        if (!(e.kind() == 1 && e.bits() == 8)) { return -5; }\n"
        "        DType f = DType.of<float8e4m3>();\n"
        "        if (!(f.kind() == 3 && f.bits() == 8 && f.variant() == 2)) { return -6; }\n"
        "        DType g = DType.of<boolean>();\n"
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
