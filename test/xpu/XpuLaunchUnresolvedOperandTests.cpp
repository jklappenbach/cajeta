// A kernel launch with an unresolved kernel name, launch operand or kernel
// argument must report a diagnostic. Found 2026-09-26 by the doc-snippet
// cleanup: an unresolved `grid: [g]` or kernel argument SIGSEGVed launch
// codegen at fault address 0x8, and a launch whose kernel name did not
// resolve but whose operands did compiled with no diagnostic at all.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public final class M {
    @Kernel
    public static void plainK(KernelBuffer<int32> out) {
        uint32 t = KernelThread.globalIdX();
        out[t] = 7;
    }
)CJ";

std::string program(const std::string& runBody) {
    return std::string(kPrelude) +
        "    public static int32 run() {\n"
        "        KernelStream s #= KernelStream.current();\n"
        "        KernelBuffer<int32> pout = heap KernelBuffer<int32>(16);\n"
        + runBody +
        "        return 0;\n"
        "    }\n"
        "}\n";
}

std::string diagnosticFor(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    try {
        auto jit = CajetaJit::compile(src, "test.M", o);
        return "<compiled>";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    }
}

}  // namespace

TEST(XpuLaunchUnresolvedOperand, resolvedLaunchIsTheControl) {
    EXPECT_EQ(diagnosticFor(program(
        "        plainK.launch(s, grid: [1], block: [16])(pout);\n"
        "        s.sync();\n")), "<compiled>");
}

TEST(XpuLaunchUnresolvedOperand, unresolvedKernelNameIsADiagnostic) {
    EXPECT_NE(diagnosticFor(program(
        "        nosuchkernel.launch(s, grid: [1], block: [16])(pout);\n")), "<compiled>");
}

TEST(XpuLaunchUnresolvedOperand, unresolvedGridOperandIsADiagnostic) {
    EXPECT_NE(diagnosticFor(program(
        "        plainK.launch(s, grid: [g], block: [16])(pout);\n")), "<compiled>");
}

TEST(XpuLaunchUnresolvedOperand, unresolvedKernelArgumentIsADiagnostic) {
    EXPECT_NE(diagnosticFor(program(
        "        plainK.launch(s, grid: [1], block: [16])(nosuchbuffer);\n")), "<compiled>");
}
