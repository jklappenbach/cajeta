// cpu-isa-variants 5.3.3.8: `KernelBuffer.prefetch(index)`, a kernel verb that
// asks for the cache line of element `index` ahead of its use. On the cpu backend
// it is llvm.prefetch (read, high locality, data cache); it moves no bytes and
// changes no result. Measured on proton: cajeta-llm's 16-row decode kernels eat a
// cache line a load and the hardware prefetchers fall behind, a third of their
// lines arriving on demand against 3.6% for the 8-row kernels.

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

const char* kWithPrefetch = R"CJ(
package pf;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
public class P {
    @Kernel
    public static void touch(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        x.prefetch((int64) i + 64L);
        x.prefetch((int64) i + 128L);
        if (i < n) { y[i] = x[i] * 2.0f; }
    }
    public static void run() { return; }
}
)CJ";

const char* kWithout = R"CJ(
package pf;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
public class P {
    @Kernel
    public static void touch(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[i] = x[i] * 2.0f; }
    }
    public static void run() { return; }
}
)CJ";

// The device IR of `source` compiled for the cpu backend, stdlib modules left out.
std::string cpuIrOf(const char* source, const char* tag) {
    auto base = fs::temp_directory_path() / ("cajeta-prefetch-" + std::string(tag) + "-"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(base / "src" / "pf");
    fs::create_directories(base / "build");
    std::ofstream(base / "src" / "pf" / "P.cajeta") << source;
    cajeta::Compiler compiler;
    compiler.setEmitMode(cajeta::EmitMode::IR);
    compiler.setXpuBackend(cajeta::XpuBackend::Cpu);
    compiler.compile("pf.P.run", (base / "src").string(), (base / "build").string());
    std::string ir;
    for (auto& e : fs::recursive_directory_iterator(base / "build"))
        if (e.is_regular_file() && e.path().extension() == ".ll"
                && e.path().filename().string().find("__stdlib__") == std::string::npos) {
            std::ifstream in(e.path(), std::ios::binary);
            std::ostringstream ss; ss << in.rdbuf();
            ir += ss.str();
        }
    fs::remove_all(base);
    return ir;
}

size_t count(const std::string& s, const std::string& needle) {
    size_t n = 0;
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1)) ++n;
    return n;
}

// The text of one defined function, `define ... @name(` to its closing brace.
std::string functionText(const std::string& ir, const std::string& name) {
    size_t at = ir.find("@" + name + "(");
    while (at != std::string::npos) {
        size_t line = ir.rfind('\n', at);
        line = line == std::string::npos ? 0 : line + 1;
        if (ir.compare(line, 6, "define") == 0) {
            size_t end = ir.find("\n}\n", at);
            return ir.substr(line, end == std::string::npos ? std::string::npos : end + 3 - line);
        }
        at = ir.find("@" + name + "(", at + 1);
    }
    return "";
}

} // namespace

// FIRES: two prefetch calls in the kernel are two llvm.prefetch calls in the
// per-work-item kernel's cpu IR (the block wrapper inlines it and carries two
// more), read hints into the data cache with the highest locality.
TEST(XpuCpuPrefetchVerb, aPrefetchLowersToLlvmPrefetchOnCpu) {
    const std::string ir = cpuIrOf(kWithPrefetch, "on");
    ASSERT_FALSE(ir.empty()) << "the kernel did not compile for cpu";
    const std::string fn = functionText(ir, "__cajeta_xpu_cpu.pf.P.touch");
    ASSERT_FALSE(fn.empty()) << "no per-work-item kernel in the IR";
    EXPECT_EQ(count(fn, "call void @llvm.prefetch"), 2u) << fn;
    EXPECT_EQ(count(fn, "i32 0, i32 3, i32 1)"), 2u) << "read, locality 3, data cache";
    EXPECT_EQ(count(ir, "call void @llvm.prefetch"), 4u) << "kernel plus its block wrapper";
}

// DOES NOT FIRE: a kernel without the verb carries no prefetch.
TEST(XpuCpuPrefetchVerb, aKernelWithoutTheVerbHasNone) {
    const std::string ir = cpuIrOf(kWithout, "off");
    ASSERT_FALSE(ir.empty());
    EXPECT_EQ(count(ir, "llvm.prefetch"), 0u);
}
