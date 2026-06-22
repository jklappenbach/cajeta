//
// Step 2 — xpu.core Cajeta stdlib declarations.
//
// Verifies that the cajeta.gpu stdlib package (KernelStream, Event,
// Fence, KernelBuffer<T>, address-space markers, capability traits, KernelThread/
// Workgroup/Barrier/Wave builtins, KernelArg, KernelError) is parsed
// into the compiler's structure table by the embedded-stdlib pass,
// and that user code can import + reference these types without
// triggering a compile failure.
//
// We don't *call* the @Native methods here — those run through the
// runtime stubs in cajeta_runtime.c (no-ops in v1). What this file
// asserts is registration and reference-ability, which is exactly
// what step 2 ships.
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::CajetaClassPtr;

namespace {

// Same pattern as AnnotationParsingTests / XpuAttributesTests:
// direct Compiler use, no JIT, so we can inspect parsed structures.
CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_core_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_core_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);

    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

// Helper: locate a class in any of the compiler's modules by
// canonical name. The xpu.core stdlib gets parsed as its own
// modules (one per .cajeta file) which aren't the user's primary
// module, so the user-module's getStructures() doesn't see them.
CajetaClassPtr findStdlibClass(Compiler& compiler,
                               const std::string& canonical) {
    for (auto& m : compiler.getModules()) {
        auto& structs = m->getStructures();
        auto it = structs.find(canonical);
        if (it != structs.end() && it->second) return it->second;
    }
    return nullptr;
}

} // namespace

// The eight non-generic xpu.core classes are registered with the
// canonical names matching their `package` + declared class name.
// Asserting registration validates that the embedded-stdlib pass
// parsed them; cross-checking the canonical name guards against a
// stray rename slipping past the prescan.
TEST(XpuCoreStdlibTests, nonGenericClassesRegistered) {
    Compiler compiler;
    // Compile a no-op user source just to bring the compiler up;
    // the stdlib parse pass runs as a side effect.
    compileForInspection(compiler,
        "package test;\npublic class T { }\n", "test.T");

    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.KernelStream"),       nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Event"),        nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Fence"),        nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.KernelThread"),       nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Workgroup"),    nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Barrier"),      nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Wave"),         nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.XpuKernelError"), nullptr);
}

// User code can import + reference KernelStream as a field type. This is
// the smoke test that the stdlib parses cleanly enough to be
// consumed by ordinary user code.
TEST(XpuCoreStdlibTests, userCodeCanReferenceStream) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.KernelStream;\n"
        "public class K {\n"
        "    KernelStream s;\n"
        "    public K(KernelStream s) {\n"
        "        this.s = s;\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
}

// Generic KernelBuffer<T> instantiation. KernelBuffer<float32> synthesizes a
// concrete class; its @Native methods emit forwarders that resolve
// to the void*-shape stubs in cajeta_runtime.c.
TEST(XpuCoreStdlibTests, bufferGenericInstantiates) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.KernelBuffer;\n"
        "public class K {\n"
        "    KernelBuffer<float32> buf;\n"
        "    public K(KernelBuffer<float32> buf) {\n"
        "        this.buf = buf;\n"
        "    }\n"
        "    public uint64 size() {\n"
        "        return this.buf.length();\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src, "test.K");
    auto klass = module->getStructures()["test.K"];
    ASSERT_NE(klass, nullptr);
    // Compile success itself proves KernelBuffer<float32> instantiated:
    // a KernelBuffer<T> field type, ctor parameter, and the buf.length()
    // call all reference the synthesized concrete class. If the
    // instantiation had failed we'd never reach this assertion.
}

// Address-space marker types are registered as ordinary generic
// classes. Step 5 wires the compiler-side recognition; for now the
// type names just exist in the namespace.
TEST(XpuCoreStdlibTests, addressSpaceMarkersRegistered) {
    Compiler compiler;
    compileForInspection(compiler,
        "package test;\npublic class T { }\n", "test.T");

    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Global"),   nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Shared"),   nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Constant"), nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Private"),  nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.Generic"),  nullptr);
}

// Capability traits are interfaces; same registration model as
// classes. The bounds-aware monomorphization that consumes these
// lands in step 9+ (Target-resolved-at-codegen) — for now we just
// confirm the trait shells exist.
TEST(XpuCoreStdlibTests, capabilityTraitsRegistered) {
    Compiler compiler;
    compileForInspection(compiler,
        "package test;\npublic class T { }\n", "test.T");

    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.TensorCoreF16"),   nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.TensorCoreBF16"),  nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.TensorCoreFP8"),   nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.WaveBallot"),      nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.WaveShuffle"),     nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.AsyncCopy"),       nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.AtomicFloatAdd"),  nullptr);
    EXPECT_NE(findStdlibClass(compiler, "cajeta.gpu.KernelArg"),       nullptr);
}
