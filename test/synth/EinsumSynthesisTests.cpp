//
// Unit 6 of the source-synthesis facility
// (docs/specification/nucleo/source-synthesis-spec.md §4.1-4.2, §6.1; plan
// agents/cajeta/nucleo/source-synthesis-plan.md §6). `@Einsum` — body
// synthesis at declaration time: a validated contraction-spec string on a
// declared-but-bodyless method synthesizes a loop-nest body over checked
// `Tensor` primitives (Tier A — source only, never IR).
//
// Validate-first (spec §6.1): the spec string is checked against the resolved
// signature — group count vs parameter count, output labels ⊆ input labels,
// Tensor-typed parameters/return with one element type, and STATIC rank where
// the declared type carries one (Matrix<E,R,C> is const-generic rank-2;
// Tensor<E> rank is runtime by design, tensor-spec.md, so it validates at the
// dynamic boundary) — all raised as user-phrased compile errors BEFORE any
// body is generated.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n";

// Compile expecting a validate-first rejection; return the diagnostic text.
std::string compileExpectError(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a validate-first compile error";
    return "";
}

} // namespace

// 6.1.1 — @Einsum("ij,jk->ik") on a bodyless matmul synthesizes a body that
// computes the contraction; result matches the reference matrix product.
// a = [[1,2],[3,4]], b = [[5,6],[7,8]] -> [[19,22],[43,50]].
TEST(EinsumSynthesisTests, matmulContractionMatchesReference) {
    std::string src = std::string(PRE) +
        "public class Ops {\n"
        "    @Einsum(\"ij,jk->ik\")\n"
        "    public static #Tensor<float32> mm(Tensor<float32> a, Tensor<float32> b);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 2;\n"
        "        Tensor<float32> a = Tensor.zeros<float32>(shp);\n"
        "        a.set2(0, 0, 1.0f); a.set2(0, 1, 2.0f);\n"
        "        a.set2(1, 0, 3.0f); a.set2(1, 1, 4.0f);\n"
        "        Tensor<float32> b = Tensor.zeros<float32>(shp);\n"
        "        b.set2(0, 0, 5.0f); b.set2(0, 1, 6.0f);\n"
        "        b.set2(1, 0, 7.0f); b.set2(1, 1, 8.0f);\n"
        "        Tensor<float32> c = Ops.mm(a, b);\n"
        "        if (c.get2(0, 0) != 19.0f) { return -1; }\n"
        "        if (c.get2(0, 1) != 22.0f) { return -2; }\n"
        "        if (c.get2(1, 0) != 43.0f) { return -3; }\n"
        "        if (c.get2(1, 1) != 50.0f) { return -4; }\n"
        "        return (int32)(c.get2(0, 0) + c.get2(1, 1));\n"  // 69
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 69);
}

// 6.1.1 (non-matmul shape) — a reduction contraction "ij->i" (row sums)
// exercises the general loop-nest path, not a matmul special case.
TEST(EinsumSynthesisTests, rowSumContraction) {
    std::string src = std::string(PRE) +
        "public class Ops {\n"
        "    @Einsum(\"ij->i\")\n"
        "    public static #Tensor<float32> rowsum(Tensor<float32> a);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 3;\n"
        "        Tensor<float32> a = Tensor.zeros<float32>(shp);\n"
        "        a.set2(0, 0, 1.0f); a.set2(0, 1, 2.0f); a.set2(0, 2, 3.0f);\n"
        "        a.set2(1, 0, 4.0f); a.set2(1, 1, 5.0f); a.set2(1, 2, 6.0f);\n"
        "        Tensor<float32> s = Ops.rowsum(a);\n"
        "        if (s.ndim() != 1) { return -1; }\n"
        "        if (s.get1(0) != 6.0f) { return -2; }\n"
        "        if (s.get1(1) != 15.0f) { return -3; }\n"
        "        return (int32)(s.get1(0) * 10.0f + s.get1(1));\n"  // 75
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 75);
}

// 6.1.2 — validate-first: a spec demanding a rank the declared argument lacks.
// Matrix<E,R,C> is const-generic rank-2 (the static-rank carrier available
// today); "bhqd,bhkd->bhqk" demands rank-4. The diagnostic is user-phrased —
// parameter name + declared spec — and no body is generated (spec 4.2, 6.1).
TEST(EinsumSynthesisTests, staticRankMismatchIsValidateFirstError) {
    std::string src = std::string(PRE) +
        "public class Ops {\n"
        "    @Einsum(\"bhqd,bhkd->bhqk\")\n"
        "    public static #Tensor<float32> att(Tensor<float32> q, Matrix<float32,2,2> k);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg = compileExpectError(src);
    EXPECT_NE(msg.find("bhqd,bhkd->bhqk"), std::string::npos) << msg;
    EXPECT_NE(msg.find("rank-4"), std::string::npos) << msg;
    EXPECT_NE(msg.find("'k'"), std::string::npos) << msg;
    EXPECT_NE(msg.find("rank-2"), std::string::npos) << msg;
}

// 6.1.2 — validate-first: input group count must match the parameter count.
TEST(EinsumSynthesisTests, groupCountMismatchIsValidateFirstError) {
    std::string src = std::string(PRE) +
        "public class Ops {\n"
        "    @Einsum(\"ij,jk->ik\")\n"
        "    public static #Tensor<float32> mm(Tensor<float32> a);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg = compileExpectError(src);
    EXPECT_NE(msg.find("ij,jk->ik"), std::string::npos) << msg;
    EXPECT_NE(msg.find("2"), std::string::npos) << msg;   // 2 groups
    EXPECT_NE(msg.find("1"), std::string::npos) << msg;   // 1 parameter
}

// 6.1.2 — validate-first: every output label must appear in some input group.
TEST(EinsumSynthesisTests, unknownOutputLabelIsValidateFirstError) {
    std::string src = std::string(PRE) +
        "public class Ops {\n"
        "    @Einsum(\"ij,jk->iz\")\n"
        "    public static #Tensor<float32> mm(Tensor<float32> a, Tensor<float32> b);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg = compileExpectError(src);
    EXPECT_NE(msg.find("'z'"), std::string::npos) << msg;
    EXPECT_NE(msg.find("ij,jk->iz"), std::string::npos) << msg;
}

// 6.1.3 — the synthesized body re-checks and codegens as ordinary code calling
// only existing Tensor primitives: the method's define exists in the emitted IR
// and its work goes through Tensor's own checked accessors.
TEST(EinsumSynthesisTests, synthesizedBodyIsOrdinaryCheckedCode) {
    std::string src = std::string(PRE) +
        "public class Ops {\n"
        "    @Einsum(\"ij,jk->ik\")\n"
        "    public static #Tensor<float32> mm(Tensor<float32> a, Tensor<float32> b);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] shp = heap int64[2];\n"
        "        shp[0] = 2;\n"
        "        shp[1] = 2;\n"
        "        Tensor<float32> a = Tensor.ones<float32>(shp);\n"
        "        Tensor<float32> b = Tensor.ones<float32>(shp);\n"
        "        Tensor<float32> c = Ops.mm(a, b);\n"
        "        return (int32) c.get2(0, 0);\n"  // 2 (1*1 + 1*1)
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    const std::string& ir = jit->getModuleIr();
    ASSERT_FALSE(ir.empty());
    // The bodyless declaration became a real defined function...
    EXPECT_NE(ir.find("test.Ops::mm"), std::string::npos);
    // ...whose body drives Tensor's own primitives (no bespoke IR emission).
    EXPECT_NE(ir.find("cajeta.math.Tensor"), std::string::npos);
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 2);
}
