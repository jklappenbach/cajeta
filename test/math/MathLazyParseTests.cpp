//
// MathLazyParseTests — numpy-porting-plan.md Phase 1: lazy stdlib parsing +
// the cajeta.math skeleton.
//
// cajeta.math (the numpy-equivalent) is embedded in the compiler but parsed
// ON DEMAND: a program that never references it costs nothing at compile time,
// while an `import cajeta.math.X` (or a hardcoded cajeta.math type such as
// Matrix) triggers exactly that package's parse. These tests pin that
// behavior via the compiler's parsed-package instrumentation
// (Compiler::stdlibPackageParsed / stdlibParsedPackages).
//
// Process model: the default JIT test path constructs a fresh Compiler per
// compile, whose ctor resets globals and whose stdlib parse resets the lazy
// bookkeeping — so each compile starts from a clean "nothing-on-demand-parsed"
// state and these probes read the result of the most recent compile.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Compiler.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::Compiler;

namespace {

// A minimal compilation unit that imports nothing from cajeta.math and never
// names a cajeta.math type. Compiling it must not parse cajeta.math.
const char* LANG_ONLY_SRC =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

} // namespace

// 1a — a program importing only cajeta.lang does NOT parse cajeta.math.
TEST(MathLazyParseTests, lazyStdlibParseSkipsUnusedPackages) {
    auto jit = CajetaJit::compile(LANG_ONLY_SRC, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);

    // The eager core still loaded (sanity that the probe reflects real state),
    // but the on-demand numpy-equivalent did not.
    EXPECT_TRUE(Compiler::stdlibPackageParsed("cajeta.lang"));
    EXPECT_FALSE(Compiler::stdlibPackageParsed("cajeta.math"));
}

// 1b — a large cajeta.math costs a non-importing program nothing. The
// timing-based form ("cold-compile time unchanged with N stub files") is
// flaky; this is its stronger, deterministic structural form: a non-importing
// program parses ZERO cajeta.math packages, so however many files the package
// accretes (Tensor, the op library, the nested submodules) they are never
// reached unless imported.
TEST(MathLazyParseTests, compilerStartupUnchangedWithLargeMathPackage) {
    CajetaJit::compile(LANG_ONLY_SRC, "test.D");

    for (const std::string& pkg : Compiler::stdlibParsedPackages()) {
        EXPECT_FALSE(pkg == "cajeta.math" || pkg.rfind("cajeta.math.", 0) == 0)
            << "non-importing program parsed on-demand package: " << pkg;
    }
}

// 1c — `import cajeta.math.X` triggers exactly that package's parse, and the
// imported type is fully usable (parsed + laid out + codegen'd end-to-end).
TEST(MathLazyParseTests, importedMathPackageParsesOnDemand) {
    std::string src =
        "package test;\n"
        "import cajeta.math.MathInfo;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return MathInfo.version();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);   // MathInfo.version() == 1

    EXPECT_TRUE(Compiler::stdlibPackageParsed("cajeta.math"));
}

// The hardcoded cajeta.math type Matrix also triggers the package's on-demand
// parse with no explicit import — its operator/method surface (resolved at
// codegen from the parsed cajeta.math.Matrix class) must be available. Guards
// the no-test-edits path for existing bare-Matrix code.
TEST(MathLazyParseTests, bareMatrixReferenceTriggersLazyParse) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Matrix<float32,2,2> b = stack Matrix<float32,2,2>(5.0f, 6.0f, 7.0f, 8.0f);\n"
        "        Matrix<float32,2,2> c = a + b;\n"
        "        return (int32) c[0][0];\n"   // 1 + 5 = 6
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 6);

    EXPECT_TRUE(Compiler::stdlibPackageParsed("cajeta.math"));
}
