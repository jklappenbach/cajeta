//
// Step 3 — KernelArg parameter validation.
//
// @Kernel methods may only take parameters whose types satisfy the
// KernelArg trait (CajetaXPU.md §3.1.1). v1's admissible set:
//
//   - primitives (anything with PRIMITIVE_FLAG)
//   - cajeta.gpu.Buffer<T> (any T)
//   - POD structs by value (a class with only primitive fields and no
//     inheritance) — admitted without a marker interface (Item 7)
//   - user types declared `implements KernelArg`
//
// The validation runs at the start of Method::generateCode for any
// method tagged @Kernel and throws cajeta::Exception (errorId
// XPU-K01) on the first non-admissible parameter found. These tests
// exercise the positive cases (admissible types compile) and the
// negative cases (inadmissible types throw with a clear message).
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/error/Exception.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_karg_" + std::to_string(rng()));
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
                 / ("cajeta_xpu_karg_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);

    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

// The validation fires at Method::generateCode time, not at parse
// time. compileForInspection only runs the parser; for the validation
// to actually fire we need to drive codegen too. Calls getAllMethods()
// + generateCode() the same way the JIT loop does.
void compileAndCodegen(Compiler& compiler,
                       const std::string& source,
                       const std::string& fqClassName) {
    auto module = compileForInspection(compiler, source, fqClassName);
    // Run the same phase-1/phase-2 pump that CajetaJit::compile uses
    // so per-method generateCode fires (and the @Kernel validator
    // with it). The loop is idempotent — re-running generateCode on
    // an already-emitted method is a cheap no-op.
    for (auto& m : compiler.getModules()) {
        for (auto& method : m->getAllMethods()) {
            method->getLlvmFunctionType();
        }
    }
    for (auto& m : compiler.getModules()) {
        for (auto& method : m->getAllMethods()) {
            method->generateCode();
        }
    }
}

} // namespace

// Primitives are admissible — int32, uint32, float32, bool all pass.
TEST(XpuKernelArgTests, primitivesAdmissible) {
    auto src =
        "package test;\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(int32 a, uint32 b, float32 c, boolean d) { }\n"
        "}\n";
    Compiler compiler;
    EXPECT_NO_THROW(compileAndCodegen(compiler, src, "test.K"));
}

// Buffer<T> is admissible for any T, by canonical-name prefix match.
TEST(XpuKernelArgTests, bufferTypeAdmissible) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.Buffer;\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(Buffer<float32> y, Buffer<float32> x,\n"
        "                           float32 a, uint32 n) { }\n"
        "}\n";
    Compiler compiler;
    EXPECT_NO_THROW(compileAndCodegen(compiler, src, "test.K"));
}

// Texture2D + Sampler (Item 8) are admissible kernel args, matched by name.
// Sampler is structurally a POD struct but must be admitted via the sampler
// path, not rejected and not treated as by-value POD.
TEST(XpuKernelArgTests, textureAndSamplerAdmissible) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.Buffer;\n"
        "import cajeta.gpu.Texture2D;\n"
        "import cajeta.gpu.Sampler;\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(Texture2D tex, Sampler s,\n"
        "                           Buffer<float32> out, uint32 n) { }\n"
        "}\n";
    Compiler compiler;
    EXPECT_NO_THROW(compileAndCodegen(compiler, src, "test.K"));
}

// User class implementing KernelArg is admissible.
TEST(XpuKernelArgTests, userTypeImplementingKernelArgAdmissible) {
    auto src =
        "package test;\n"
        "import cajeta.gpu.KernelArg;\n"
        "public class MyPod implements KernelArg {\n"
        "    int32 a;\n"
        "    int32 b;\n"
        "    public MyPod(int32 a, int32 b) { this.a = a; this.b = b; }\n"
        "}\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(MyPod p) { }\n"
        "}\n";
    Compiler compiler;
    EXPECT_NO_THROW(compileAndCodegen(compiler, src, "test.K"));
}

// A plain POD struct (all-primitive fields, no inheritance, no marker
// interface) is admissible by value as a kernel arg (Item 7).
TEST(XpuKernelArgTests, podStructAdmissible) {
    auto src =
        "package test;\n"
        "public class Params {\n"
        "    float32 scale;\n"
        "    int32 bias;\n"
        "    public Params(float32 scale, int32 bias)"
        " { this.scale = scale; this.bias = bias; }\n"
        "}\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(Params p) { }\n"
        "}\n";
    Compiler compiler;
    EXPECT_NO_THROW(compileAndCodegen(compiler, src, "test.K"));
}

// A NON-POD class (it has a class-typed, non-primitive field) without the
// KernelArg marker is still rejected with XPU-K01.
TEST(XpuKernelArgTests, nonPodUserTypeRejected) {
    auto src =
        "package test;\n"
        "public class Inner {\n"
        "    int32 x;\n"
        "    public Inner(int32 x) { this.x = x; }\n"
        "}\n"
        "public class NotKernelArg {\n"
        "    Inner inner;\n"
        "    public NotKernelArg(Inner inner) { this.inner = inner; }\n"
        "}\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(NotKernelArg p) { }\n"
        "}\n";
    Compiler compiler;
    try {
        compileAndCodegen(compiler, src, "test.K");
        FAIL() << "expected cajeta::Exception XPU-K01";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "XPU-K01");
        EXPECT_NE(e.getMessage().find("not admissible"), std::string::npos);
        EXPECT_NE(e.getMessage().find("NotKernelArg"), std::string::npos);
    }
}

// Validation is gated on @Kernel — a non-kernel method taking a
// non-admissible type compiles fine.
TEST(XpuKernelArgTests, nonKernelMethodNotValidated) {
    auto src =
        "package test;\n"
        "public class Inner {\n"
        "    int32 x;\n"
        "    public Inner(int32 x) { this.x = x; }\n"
        "}\n"
        "public class NotKernelArg {\n"
        "    Inner inner;\n"
        "    public NotKernelArg(Inner inner) { this.inner = inner; }\n"
        "}\n"
        "public class K {\n"
        "    public static void notAKernel(NotKernelArg p) { }\n"
        "}\n";
    Compiler compiler;
    EXPECT_NO_THROW(compileAndCodegen(compiler, src, "test.K"));
}
