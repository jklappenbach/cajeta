//
// A device intrinsic called from HOST code.
//
// `cajeta.xpu.CooperativeMatrix`'s verbs are declared with REAL EMPTY
// BODIES. That is deliberate and load-bearing: the kernel lowering
// intercepts them by receiver type and method name and emits the
// instruction at the call site, so the body is never compiled and never
// needs to exist. Inside an @Kernel that is exactly right.
//
// Outside one, the body IS what runs. `mc.mma(a, b)` from host code
// compiled, returned, and did nothing — no diagnostic, no effect, an
// accumulator left holding whatever it held. I found this while answering
// whether `abstract` could mark these methods (it cannot: the class is
// `final` and a kernel DECLARES one, so making the methods abstract would
// make the class abstract and break the only way the type is used).
//
// It has never bitten, because these types are only reachable from kernel
// code in practice. That is luck, not a guarantee, and it is the same
// invisible-absence shape as a vacuous pass: the call is there, it reads
// as work, and it does nothing.
//
// THE MARKER IS `@Intrinsic`, AND THE FIRST VERSION OF THIS USED `@Device`,
// WHICH WAS WRONG. @Device marks a method as ALSO usable on the device; it
// does not mean device-only. `GgufFile.halfBitsToF32` is @Device, has a
// real body, and is called from HOST code at GgufFile.cajeta:417 — so a
// device-only rule rejects cajeta-llm outright.
//
// Nothing in the cajeta suite would have caught that: cajeta-llm is a
// different repository, and the only @Device methods in THIS tree were the
// fifteen I had just annotated, which call nothing. The blast radius of a
// check on every method call in the language does not live where the check
// does. Measured 2026-09-21 by reading the consumers before trusting a
// green sweep.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile for the CPU backend so the kernel path is real on any box; the
// rejection is a front-end check and fires before any device work.
std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    try {
        CajetaJit::compile(src, "test.D", o);
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode) << e.getMessage();
        return e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "wrong exception type: " << e.what();
        return e.what();
    }
    ADD_FAILURE() << "expected " << expectCode;
    return "";
}

void compileOk(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    try {
        EXPECT_NE(CajetaJit::compile(src, "test.D", o), nullptr);
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "must compile, got " << e.getErrorId() << ": "
                      << e.getMessage();
    }
}

const char* PRE =
    "package test;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Shared;\n";

}  // namespace

// FIRES: the verb called from an ordinary host method.
TEST(XpuDeviceOnlyCallTests, aCoopVerbCalledFromHostCodeIsRefused) {
    std::string msg = compileExpectError(
        std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // An explicit allocation, not a bare declaration. A bare
        // `CooperativeMatrix<...> acc;` on the host is ALREADY refused, by
        // the uninitialized-variable check — so the hole is narrower than
        // it first looked, and this is the spelling that actually reaches
        // the empty body. Measured 2026-09-20 while writing this test.
        "        CooperativeMatrix<float32,16,16,2> acc #=\n"
        "            heap CooperativeMatrix<float32,16,16,2>();\n"
        "        acc.splat(0.0f);\n"
        "        return 0;\n"
        "    }\n"
        "}\n",
        "CAJETA_ERROR_INTRINSIC_CALLED_ON_HOST");
    EXPECT_NE(msg.find("splat"), std::string::npos)
        << "the refusal must NAME the verb:\n" << msg;
    EXPECT_NE(msg.find("@Kernel"), std::string::npos)
        << "and say where the call belongs:\n" << msg;
}

// DOES NOT FIRE: the same verb inside a kernel, which is the whole point.
// Without this, marking every intrinsic @Device and rejecting every call
// would also "pass" the test above.
TEST(XpuDeviceOnlyCallTests, theSameVerbInsideAKernelIsFine) {
    compileOk(std::string(PRE) +
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void k(KernelBuffer<float32> out) {\n"
        "        CooperativeMatrix<float32,16,16,2> acc;\n"
        "        acc.splat(0.0f);\n"
        "        acc.store(out, 0, 0, 16);\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n");
}

// DOES NOT FIRE for ordinary host calls. The check reads one annotation;
// a bug in that read would take the whole language down with it, and this
// is the cheapest possible canary for that.
TEST(XpuDeviceOnlyCallTests, ordinaryHostCallsAreUntouched) {
    compileOk(
        "package test;\n"
        "public final class Helper {\n"
        "    public Helper() { return; }\n"
        "    public int32 twice(int32 x) { return x * 2; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Helper h #= heap Helper();\n"
        "        return h.twice(21);\n"
        "    }\n"
        "}\n");
}

// FIRES on a SECOND intrinsic type, to prove the check keys on the resolved
// method's @Intrinsic and not on anything specific to CooperativeMatrix.
// Schedule.barrier is a static scheduling-hint intrinsic (lowered to
// llvm.amdgcn.sched.barrier and friends inside a kernel); from host code it
// has nothing to run. Same mechanism, different type — the whole point of
// annotating the family rather than special-casing one class.
TEST(XpuDeviceOnlyCallTests, aScheduleIntrinsicFromHostCodeIsRefused) {
    std::string msg = compileExpectError(
        "package test;\n"
        "import cajeta.xpu.Schedule;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Schedule.barrier(0);\n"
        "        return 0;\n"
        "    }\n"
        "}\n",
        "CAJETA_ERROR_INTRINSIC_CALLED_ON_HOST");
    EXPECT_NE(msg.find("barrier"), std::string::npos)
        << "the refusal must NAME the verb:\n" << msg;
    EXPECT_NE(msg.find("@Kernel"), std::string::npos)
        << "and say where it belongs:\n" << msg;
}

// DOES NOT FIRE inside a kernel whose device-only parameter is not a
// KernelBuffer. The host-body stub was keyed on that one type, so an Image2D
// kernel had its real body lowered as host code and every intrinsic in it was
// refused. The kernel arm above passes only because it takes a buffer.
TEST(XpuDeviceOnlyCallTests, aKernelTakingAnImageIsFine) {
    compileOk(
        "package test;\n"
        "import cajeta.xpu.Image2D;\n"
        "import cajeta.xpu.KernelThread;\n"
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void k(Image2D img, uint32 w, uint32 h) {\n"
        "        uint32 i = KernelThread.globalIdX();\n"
        "        if (i < w * h) {\n"
        "            float32 v = img.load(i % w, i / w);\n"
        "            img.store(i % w, i / w, 2.0f * v + 1.0f);\n"
        "        }\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n");
}
