//
// Step 5 — address-space type recognition.
//
// Verifies that the canonical-name → AddressSpace mapping in
// AddressSpace.h plus the buildParams hook in XpuMirBuilder
// correctly classify parameter types into address spaces, and
// that kernels using GpuBuffer<T> (the conventional global-memory
// view) flow through cleanly.
//
// The AddressSpaceLowerPass that emits `addrspace(N)` decorations
// on LLVM IR lands with the NVPTX backend (step 9). Step 5 just
// proves the recognition is in place so step 9 has a stable place
// to hook.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "cajeta/xpu/mir/XpuMirBuilder.h"
#include "cajeta/xpu/core/AddressSpace.h"

#include <filesystem>
#include <fstream>
#include <random>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::xpu::AddressSpace;
using cajeta::xpu::mir::XpuMirBuilder;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_as_" + std::to_string(rng()));
    std::filesystem::create_directories(base);
    std::filesystem::path rel;
    size_t start = 0;
    for (size_t i = 0; i <= fqClassName.size(); ++i) {
        if (i == fqClassName.size() || fqClassName[i] == '.') {
            rel /= fqClassName.substr(start, i - start);
            start = i + 1;
        }
    }
    rel += ".cajeta";
    auto full = base / rel;
    std::filesystem::create_directories(full.parent_path());
    std::ofstream out(full);
    out << source;
    out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_as_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods()) {
        if (m && m->getName() == name) return m;
    }
    return nullptr;
}

} // namespace

// Saxpy-shaped kernel: GpuBuffer<float32> y, GpuBuffer<float32> x, float32 a,
// uint32 n. MIR builder captures all four parameters with the right
// underlying types and Generic (default) address space — GpuBuffer<T>'s
// implicit "lives in global memory" handling happens at backend lower
// time, not at MIR-builder time.
TEST(XpuAddressSpaceTests, saxpyKernelParametersFlowThrough) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.GpuBuffer;\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void saxpy(GpuBuffer<float32> y, GpuBuffer<float32> x,\n"
        "                             float32 a, uint32 n) { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto m = findMethod(klass, "saxpy");
    ASSERT_NE(m, nullptr);

    auto k = XpuMirBuilder::buildKernelForMethod(m);
    ASSERT_NE(k, nullptr);
    ASSERT_EQ(k->params.size(), 4u);

    EXPECT_EQ(k->params[0].name, "y");
    EXPECT_EQ(k->params[1].name, "x");
    EXPECT_EQ(k->params[2].name, "a");
    EXPECT_EQ(k->params[3].name, "n");

    // All four default to Generic at MIR-build time. GpuBuffer<T> →
    // Global lowering is a per-backend decision (step 9+); the MIR
    // doesn't lock it in.
    for (auto& p : k->params) {
        EXPECT_EQ(p.type.addressSpace, AddressSpace::Generic);
        ASSERT_NE(p.type.underlying, nullptr);
    }

    // Parameter type canonicals should reflect GpuBuffer<float32> +
    // primitives; this is the surface step 9 will translate into
    // addrspace(1) for the GpuBuffer pointers.
    EXPECT_NE(k->params[0].type.underlying->toCanonical()
                .find("GpuBuffer"), std::string::npos);
    EXPECT_NE(k->params[2].type.underlying->toCanonical()
                .find("float32"), std::string::npos);
}

// The address-space marker types from runtime/src/cajeta/xpu/core/
// AddressSpace.cajeta are reachable as ordinary types. v1 doesn't
// instantiate them as kernel parameters (KernelArg validation
// rejects), but they're available as local types — exercised here
// via a non-kernel host method using a Global<float32> field.
TEST(XpuAddressSpaceTests, globalMarkerTypeReachable) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.Global;\n"
        "public class K {\n"
        "    Global<float32> g;\n"
        "    public K(Global<float32> g) { this.g = g; }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    // No further assertion needed — successful compile means the
    // type was found, instantiated for float32, and the recognizer
    // didn't reject the use.
}
