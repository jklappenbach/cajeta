//
// transform-intrinsics 3.1.5 — the synthesized backward re-enters the FULL
// checked pipeline (parse -> type-check -> borrow-check -> codegen), so an
// ill-typed rule product must be REJECTED, never emitted. The probe forces a
// deliberately bad VJP rule via CAJETA_NUCLEO_FORCE_BAD_VJP=<primitive> (read
// per-lookup in VjpRegistry::lookup, one site; the builtin table stays
// immutable) whose cotangent references an undeclared function. The
// silent-resolution work is what makes this provable: the bad reference is a
// hard member-not-found in the re-checked source, not a null that codegens.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdlib>
#include <string>

using cajeta_test::CajetaJit;

namespace {
const char* kGradSrc =
    "package test;\n"
    "import cajeta.nucleo.transform.GradResult;\n"
    "public final class T {\n"
    "    public static float32 run() {\n"
    "        (float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * x);\n"
    "        GradResult<float32,float32> r = g(3.0f);\n"
    "        return r.value * 100.0f + r.grads;\n"
    "    }\n"
    "}\n";
} // namespace

// A forced ill-typed backward fails the compile with a named error — the
// re-check is real, not a formality.
TEST(GradRecheck, forcedBadRuleIsRejectedNotEmitted) {
    setenv("CAJETA_NUCLEO_FORCE_BAD_VJP", "mul", 1);
    struct Unset {
        ~Unset() { unsetenv("CAJETA_NUCLEO_FORCE_BAD_VJP"); }
    } guard;
    try {
        auto jit = CajetaJit::compile(kGradSrc, "test.T");
        (void) jit->lookup<float (*)()>("run")();
        FAIL() << "an ill-typed backward must fail the compile, not emit";
    } catch (cajeta::Exception& e) {
        EXPECT_FALSE(e.getErrorId().empty())
            << "rejection must be a NAMED error, got: " << e.getMessage();
    }
}

// The hook leaves no state behind: the same program compiles and runs
// correctly once the variable is gone.
TEST(GradRecheck, cleanCompileAfterUnset) {
    unsetenv("CAJETA_NUCLEO_FORCE_BAD_VJP");
    auto jit = CajetaJit::compile(kGradSrc, "test.T");
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 906.0f);
}
