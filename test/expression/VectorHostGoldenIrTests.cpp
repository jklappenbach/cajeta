//
// S0 of the value-type operator-overloading mechanism
// (plans/value-type-overloading-plan.md): the HOST golden-IR baseline for
// Vector<T,N>. It is the zero-regression oracle for the Vector retrofit (S9):
// Vector is a by-value value type whose operators are lowered by compiler
// INTRINSIC INTERCEPTION, not method dispatch — so its host codegen is flat
// `<N x T>` SSA (register-resident), the same shape an intrinsic emits, with NO
// call into a Vector operator method. These assertions pin that shape so the
// operator-mechanism work (and any future move of Vector onto the declared
// operator surface) cannot silently regress it.
//
// Mirrors XpuVectorDeviceTests.lowersToVectorIr (the device-side oracle) on the
// host JIT path, reading the captured post-codegen IR via Options::captureIr.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// A representative Vector<float32,4> workload: construction, element-wise add,
// scalar broadcast, component assignment + read, and the dot helper — every
// host Vector codegen path the retrofit must preserve.
std::string compileVectorWorkloadIr() {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vector<float32,4> a = new Vector<float32,4>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Vector<float32,4> b = new Vector<float32,4>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        Vector<float32,4> s = a + b;\n"       // fadd <4 x float>
        "        Vector<float32,4> t = s * 2.0f;\n"    // fmul <4 x float> (broadcast)
        "        t.x = 99.0f;\n"                        // insertelement
        "        float32 d = a.dot(b);\n"               // fmul + reduce
        "        return (int32)(t.x + t.y + d);\n"      // extractelement
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    return jit->getModuleIr();
}

void expectContains(const std::string& ir, const std::string& needle) {
    EXPECT_NE(ir.find(needle), std::string::npos)
        << "expected golden-IR substring missing: '" << needle << "'\n" << ir;
}

} // namespace

// The flat register-resident vector shape: a `<4 x float>` value type carried in
// SSA, element-wise float arithmetic as vector ops, scalar broadcast as a vector
// fmul, and component read/write as extract/insert — identical to the device
// oracle's assertions (XpuVectorDeviceTests.lowersToVectorIr).
TEST(VectorHostGoldenIrTests, lowersToFlatVectorIntrinsics) {
    std::string ir = compileVectorWorkloadIr();
    ASSERT_FALSE(ir.empty()) << "captureIr produced no module IR";

    expectContains(ir, "<4 x float>");
    expectContains(ir, "fadd <4 x float>");   // a + b
    expectContains(ir, "fmul <4 x float>");   // s * 2.0f broadcast
    expectContains(ir, "extractelement");      // component read
    expectContains(ir, "insertelement");       // component assign (t.x = ...)
}

// The defining invariant for the retrofit: Vector arithmetic is INTERCEPTED as
// an intrinsic, never dispatched through a Vector operator method. There must be
// no call to a `Vector<...>::operator...` in the emitted module — the `+` / `*`
// folded to inline vector SSA above. If the S9 retrofit ever routes Vector
// through the declared operator surface, this guard fails loudly.
TEST(VectorHostGoldenIrTests, vectorOperatorsAreNotDispatchedAsCalls) {
    std::string ir = compileVectorWorkloadIr();
    ASSERT_FALSE(ir.empty()) << "captureIr produced no module IR";

    EXPECT_EQ(ir.find("Vector<float32,4>::operator"), std::string::npos)
        << "Vector operator was dispatched as a method call (expected intrinsic "
           "interception to flat <N x T> SSA)\n" << ir;
}
