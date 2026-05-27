//
// Step 3 — KernelArg parameter validation.
//
// @Kernel methods may only take parameters whose types satisfy the
// KernelArg trait (CajetaXPU.md §3.1.1). v1's admissible set:
//
//   - primitives (anything with PRIMITIVE_FLAG)
//   - cajeta.xpu.core.Buffer<T> (any T)
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
        "import cajeta.xpu.core.Buffer;\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(Buffer<float32> y, Buffer<float32> x,\n"
        "                           float32 a, uint32 n) { }\n"
        "}\n";
    Compiler compiler;
    EXPECT_NO_THROW(compileAndCodegen(compiler, src, "test.K"));
}

// User class implementing KernelArg is admissible.
TEST(XpuKernelArgTests, userTypeImplementingKernelArgAdmissible) {
    auto src =
        "package test;\n"
        "import cajeta.xpu.core.KernelArg;\n"
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

// User class NOT implementing KernelArg is rejected with XPU-K01.
TEST(XpuKernelArgTests, plainUserTypeRejected) {
    auto src =
        "package test;\n"
        "public class NotKernelArg {\n"
        "    int32 a;\n"
        "    public NotKernelArg(int32 a) { this.a = a; }\n"
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
        "public class NotKernelArg {\n"
        "    int32 a;\n"
        "    public NotKernelArg(int32 a) { this.a = a; }\n"
        "}\n"
        "public class K {\n"
        "    public static void notAKernel(NotKernelArg p) { }\n"
        "}\n";
    Compiler compiler;
    EXPECT_NO_THROW(compileAndCodegen(compiler, src, "test.K"));
}
