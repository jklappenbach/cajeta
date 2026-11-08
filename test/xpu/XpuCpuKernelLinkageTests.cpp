// cajeta-ml-gpu 21.2.2: a kernel in a class template, instantiated in a library and again in a
// consumer that inlines that library's code, must link as one definition. The cpu kernel, its
// block wrapper and its launch thunk carry weak ODR linkage (v0.53.0 emitted them strong and
// ld.lld refused the duplicate when the kernels package's GemmSplit<S> met cajeta-ml).
#include "gtest/gtest.h"

#include "../PortableEnv.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

namespace {

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) {
        r = envRoot;
    } else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
#ifdef _WIN32
    std::string p = r + "/build/src/cajeta.exe";
    std::replace(p.begin(), p.end(), '/', '\\');
    return p;
#else
    return r + "/build/src/cajeta";
#endif
}

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class Twice<uint32 S> {
    public Twice() { return; }
    @Kernel
    public static void scale(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[i] = x[i] * (float32) S; }
    }
    public void run(KernelBuffer<float32> y, KernelBuffer<float32> x, uint32 n) {
        KernelStream s #= KernelStream.current();
        scale.launch(s, grid: [(n + 63) / 64], block: [64])(y, x, n);
        s.sync();
    }
}
public class M {
    public static int32 run() {
        KernelBuffer<float32> x = heap KernelBuffer<float32>(4);
        KernelBuffer<float32> y = heap KernelBuffer<float32>(4);
        Twice<2> t = heap Twice<2>();
        t.run(y, x, 4);
        return 0;
    }
}
)CJ";

} // namespace

TEST(XpuCpuKernelLinkage, aClassTemplateKernelsSymbolsAreWeakOdr) {
    namespace fs = std::filesystem;
    std::string bin = compilerBinary();
    if (!fs::exists(bin)) GTEST_SKIP() << "no compiler binary at " << bin;
    static std::mt19937_64 rng(std::random_device{}());
    fs::path root = fs::temp_directory_path() / ("cajeta_klink_" + std::to_string(rng()));
    fs::create_directories(root / "test");
    fs::create_directories(root / "out");
    { std::ofstream o(root / "test" / "M.cajeta"); o << kSource; }
    fs::path errFile = root / "stderr.txt";
    std::string cmd = bin + " --emit=ir --xpu-backend=cpu test.M.run " + root.string() + " "
        + (root / "out").string() + " > " CAJETA_PORTABLE_DEVNULL " 2> " + errFile.string();
    int rc = std::system(cmd.c_str());
    std::ifstream ein(errFile);
    std::stringstream ess; ess << ein.rdbuf();
    ASSERT_EQ(rc, 0) << ess.str();
    std::ifstream in(root / "out" / "test" / "M.ll");
    std::stringstream ss; ss << in.rdbuf();
    std::string ir = ss.str();
    EXPECT_NE(ir.find("define weak_odr void @__cajeta_xpu_cpu.test.Twice_2_.scale("), std::string::npos) << ess.str();
    EXPECT_NE(ir.find("define weak_odr void @__cajeta_xpu_cpu_block.test.Twice_2_.scale("), std::string::npos);
    EXPECT_NE(ir.find("define weak_odr void @__cajeta_xpu_cpu_launch.test.Twice_2_.scale("), std::string::npos);
    EXPECT_EQ(ir.find("define void @__cajeta_xpu_cpu_block.test.Twice_2_.scale("), std::string::npos);
    fs::remove_all(root);
}
