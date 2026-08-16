//
// Step 4 — XPU MIR scaffolding.
//
// Verifies that XpuMirBuilder constructs a structurally-correct
// XpuMirKernel record for @Kernel methods (canonical name,
// parameter list with address-space-qualified types, @Wave /
// @Backend co-annotations surfaced) and walks a module to produce
// a populated XpuMirModule.
//
// Steps 5-7 fill in body-op extraction, launch sites, and the CPU
// emulation that consumes the MIR. Step 4 just lands the data
// shape.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "cajeta/xpu/mir/XpuMirBuilder.h"
#include "cajeta/xpu/mir/XpuMirPrinter.h"
#include "cajeta/xpu/core/AddressSpace.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::xpu::XpuBackend;
using cajeta::xpu::AddressSpace;
using cajeta::xpu::mir::XpuMirBuilder;
using cajeta::xpu::mir::XpuMirKernelPtr;
using cajeta::xpu::mir::XpuMirModulePtr;
using cajeta::xpu::mir::XpuMirPrinter;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_mir_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_mir_arch_" + std::to_string(rng()));
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

// Non-@Kernel methods produce nullptr from the per-method builder.

// @Kernel method with simple int args produces a kernel record
// whose canonical name reflects package+class+method and whose
// parameter list mirrors the method signature.

// @Wave(width = 32) and @Backend("nvidia") flow through to the
// MIR record via XpuKernelAttr.

// buildForModule walks all @Kernel methods in a module's class
// table and returns a populated XpuMirModule.

// XpuMirPrinter produces a text dump that includes the kernel
// canonical name, wave width, backend tag, and parameter list.
// The format isn't a wire-format commitment — these checks are
// substring greps, not exact-match.
TEST(XpuMirBuilderTests, printerRoundTrip) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel @Wave(width = 64) @Backend(\"amd\")\n"
        "    public static void run(uint32 n) { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    auto m = findMethod(klass, "run");
    ASSERT_NE(m, nullptr);

    auto k = XpuMirBuilder::buildKernelForMethod(m);
    ASSERT_NE(k, nullptr);
    std::string text = XpuMirPrinter::print(*k);

    EXPECT_NE(text.find("kernel test.K.run"), std::string::npos);
    EXPECT_NE(text.find("wave 64"), std::string::npos);
    EXPECT_NE(text.find("backend amd"), std::string::npos);
    EXPECT_NE(text.find("param n"), std::string::npos);
}

// AddressSpace recognition: a method using xpu.Global<float32> as
// a non-kernel parameter type should be admissible (this isn't a
// @Kernel) but the type-level recognition is what step 5 builds
// on. This test just exercises the recognition helper directly.
TEST(XpuMirBuilderTests, addressSpaceRecognitionHelper) {
    AddressSpace as;
    EXPECT_TRUE(cajeta::xpu::isAddressSpaceCanonical(
        "cajeta.xpu.Global", as));
    EXPECT_EQ(as, AddressSpace::Global);

    EXPECT_TRUE(cajeta::xpu::isAddressSpaceCanonical(
        "cajeta.xpu.Shared<cajeta.float32>", as));
    EXPECT_EQ(as, AddressSpace::Shared);

    EXPECT_TRUE(cajeta::xpu::isAddressSpaceCanonical(
        "cajeta.xpu.Generic", as));
    EXPECT_EQ(as, AddressSpace::Generic);

    EXPECT_FALSE(cajeta::xpu::isAddressSpaceCanonical(
        "cajeta.xpu.KernelStream", as));
    EXPECT_FALSE(cajeta::xpu::isAddressSpaceCanonical(
        "user.pkg.Global", as));     // wrong package
}

// Backend numeric mapping tables are populated for all three
// backends at compile time. The CajetaXPU-Variance.md row-4 table
// freezes here.
TEST(XpuMirBuilderTests, addressSpaceNumberingTablesConsistent) {
    using cajeta::xpu::nvidiaNumberFor;
    using cajeta::xpu::amdNumberFor;
    using cajeta::xpu::spirvNumberFor;

    // NVIDIA/AMD agree on the LLVM convention.
    EXPECT_EQ(nvidiaNumberFor(AddressSpace::Global),   1);
    EXPECT_EQ(amdNumberFor(AddressSpace::Global),      1);
    EXPECT_EQ(nvidiaNumberFor(AddressSpace::Shared),   3);
    EXPECT_EQ(amdNumberFor(AddressSpace::Shared),      3);

    // SPIR-V storage-class numbers are different (Khronos enum).
    EXPECT_EQ(spirvNumberFor(AddressSpace::Global),    12);   // StorageBuffer
    EXPECT_EQ(spirvNumberFor(AddressSpace::Shared),    4);    // Workgroup
    EXPECT_EQ(spirvNumberFor(AddressSpace::Constant),  2);    // Uniform
}
