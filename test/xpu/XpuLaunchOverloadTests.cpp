// cajeta-ml-gpu 24.2.2: a host method with a @Kernel's name beside the kernel. On v0.53.0
// `glu.launch(...)` beside a host `glu` compiled, and the call site bound whichever won:
// the launch happened for neither, the output kept its old values, and nothing said so.
// A kernel is launched by its bare name, so the pair is refused at declaration; a host
// entry of its own name runs the kernel.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public final class D {\n"
    "    @Kernel\n"
    "    public static void glu<E>(KernelBuffer<E> out, KernelBuffer<E> g, KernelBuffer<E> u, uint32 n, int32 kind) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { out[i] = (E) ((float32) g[i] * (float32) u[i] + (float32) kind); }\n"
    "    }\n"
    "    /** The host entry of the same name and arity: it must not capture the launch. */\n"
    "    public static void glu<E>(KernelBuffer<E> out, KernelBuffer<E> g, KernelBuffer<E> u, int64 n, int32 kind) {\n"
    "        uint32 un = (uint32) n;\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        glu.launch(s, grid: [(un + 63) / 64], block: [64])(out, g, u, un, kind);\n"
    "        s.sync();\n"
    "    }\n"
    "    /** Mismatches after the host entry runs the kernel over 100 elements: 0 when the launch happened. */\n"
    "    public static int32 run() {\n"
    "        int64 n = 100L;\n"
    "        float32[] hg = heap float32[n];\n"
    "        float32[] hu = heap float32[n];\n"
    "        float32[] ho = heap float32[n];\n"
    "        int32 i = 0;\n"
    "        while (i < 100) { hg[i] = (float32) i; hu[i] = 2.0f; ho[i] = -1.0f; i = i + 1; }\n"
    "        KernelBuffer<float32> g = heap KernelBuffer<float32>((uint64) n);\n"
    "        KernelBuffer<float32> u = heap KernelBuffer<float32>((uint64) n);\n"
    "        KernelBuffer<float32> out = heap KernelBuffer<float32>((uint64) n);\n"
    "        g.upload(hg);\n"
    "        u.upload(hu);\n"
    "        out.upload(ho);\n"
    "        D.glu<float32>(out, g, u, n, 3);\n"
    "        float32[] got = heap float32[n];\n"
    "        out.download(got);\n"
    "        int32 bad = 0;\n"
    "        i = 0;\n"
    "        while (i < 100) { if (got[i] != (float32) i * 2.0f + 3.0f) { bad = bad + 1; } i = i + 1; }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

std::string renamedHostEntry() {
    std::string s = kSource;
    const std::string hostDecl = "    public static void glu<E>(KernelBuffer<E> out, KernelBuffer<E> g, KernelBuffer<E> u, int64 n, int32 kind) {";
    const std::string gatedDecl = "    public static void gated<E>(KernelBuffer<E> out, KernelBuffer<E> g, KernelBuffer<E> u, int64 n, int32 kind) {";
    size_t p = s.find(hostDecl);
    if (p != std::string::npos) s.replace(p, hostDecl.size(), gatedDecl);
    const std::string call = "D.glu<float32>(out, g, u, n, 3);";
    p = s.find(call);
    if (p != std::string::npos) s.replace(p, call.size(), "D.gated<float32>(out, g, u, n, 3);");
    return s;
}

} // namespace

// The pair is an error at declaration, naming the method and the fix.
TEST(XpuLaunchOverload, aHostMethodOfTheKernelsNameIsRefusedAtDeclaration) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    try {
        auto jit = CajetaJit::compile(kSource, "test.D", o);
        ADD_FAILURE() << "expected CAJETA_ERROR_KERNEL_NAME_SHARED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_KERNEL_NAME_SHARED");
        EXPECT_NE(e.getMessage().find("glu"), std::string::npos) << e.getMessage();
        EXPECT_NE(e.getMessage().find("bare name"), std::string::npos) << e.getMessage();
    }
}

// With a name of its own, the host entry launches the kernel and the output is right.
TEST(XpuLaunchOverload, aHostEntryOfItsOwnNameLaunchesTheKernel) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(renamedHostEntry(), "test.D", o);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_NE(jit, nullptr) << err;
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 0) << err;
}
