//
// CajetaXPU §3.5 / §11 — launch borrow scope checking.
//
// A `kernel.launch(...)(args)` borrows each Buffer argument for the duration
// of the asynchronous launch; the borrow is released at the next
// Stream.sync() / Event.waitHost(). Freeing a buffer that a launch still
// references — before syncing — is a compile error (XPU-K02): a real
// use-after-free of device memory an in-flight kernel reads/writes.
//
// The check is codegen-interleaved (like the rest of the borrow model), so
// these tests drive Phase-2 codegen and observe the diagnostic. Buffers and
// the stream are method parameters (no allocator needed yet) and the kernel
// body is empty (host codegen of the kernel itself stays trivial).
//

#include "gtest/gtest.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"
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
              / ("cajeta_xpu_bz_" + std::to_string(rng()));
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
    std::ofstream out(full); out << source; out.close();
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_bz_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

void codegenAll(Compiler& compiler) {
    for (auto& m : compiler.getModules())
        for (auto& method : m->getAllMethods())
            method->getLlvmFunctionType();
    for (auto& m : compiler.getModules())
        for (auto& method : m->getAllMethods())
            method->generateCode();
}

// Returns the errorId of the thrown cajeta::Exception, or "" if no throw.
std::string codegenErrorId(Compiler& compiler) {
    try {
        codegenAll(compiler);
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    }
    return "";
}

const char* kHeader =
    "package test;\n"
    "import cajeta.gpu.core.Buffer;\n"
    "import cajeta.gpu.core.Stream;\n";

} // namespace

// Freeing a launch-borrowed buffer before syncing is XPU-K02.
TEST(XpuLaunchBorrowTests, freeBeforeSyncRejected) {
    std::string src = std::string(kHeader) +
        "public class M {\n"
        "    @Kernel public static void k(Buffer<float32> y, uint32 n) { }\n"
        "    public static void run(Buffer<float32> y, Stream s, uint32 n) {\n"
        "        k.launch(s, grid: [1], block: [1])(y, n);\n"
        "        y.free();\n"   // ERROR: still borrowed by the launch
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.M");
    EXPECT_EQ(codegenErrorId(compiler), "XPU-K02");
}

// Syncing the stream releases the launch borrow, so the later free is fine.
TEST(XpuLaunchBorrowTests, freeAfterSyncAccepted) {
    std::string src = std::string(kHeader) +
        "public class M {\n"
        "    @Kernel public static void k(Buffer<float32> y, uint32 n) { }\n"
        "    public static void run(Buffer<float32> y, Stream s, uint32 n) {\n"
        "        k.launch(s, grid: [1], block: [1])(y, n);\n"
        "        s.sync();\n"
        "        y.free();\n"   // OK: borrow released at sync
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.M");
    EXPECT_EQ(codegenErrorId(compiler), "");
}

// A free with no launch outstanding is unaffected by the check.
TEST(XpuLaunchBorrowTests, freeWithoutLaunchAccepted) {
    std::string src = std::string(kHeader) +
        "public class M {\n"
        "    public static void run(Buffer<float32> y) {\n"
        "        y.free();\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.M");
    EXPECT_EQ(codegenErrorId(compiler), "");
}

// Letting a launch-borrowed OWNED buffer leave scope before syncing is XPU-K02
// — the implicit-drop counterpart to freeBeforeSyncRejected. With Buffer<T>'s
// RAII destructor, the scope-exit drop would free device memory an in-flight
// kernel still references.
TEST(XpuLaunchBorrowTests, dropBeforeSyncRejected) {
    std::string src = std::string(kHeader) +
        "public class M {\n"
        "    @Kernel public static void k(Buffer<float32> y, uint32 n) { }\n"
        "    public static void run(Stream s, uint32 n) {\n"
        "        Buffer<float32> y = heap Buffer<float32>(n);\n"
        "        k.launch(s, grid: [1], block: [1])(y, n);\n"
        "        // no sync — y's drop at scope exit would use-after-free\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.M");
    EXPECT_EQ(codegenErrorId(compiler), "XPU-K02");
}

// A sync before the owned buffer leaves scope releases the borrow, so the RAII
// drop is fine — no explicit free() needed.
TEST(XpuLaunchBorrowTests, dropAfterSyncAccepted) {
    std::string src = std::string(kHeader) +
        "public class M {\n"
        "    @Kernel public static void k(Buffer<float32> y, uint32 n) { }\n"
        "    public static void run(Stream s, uint32 n) {\n"
        "        Buffer<float32> y = heap Buffer<float32>(n);\n"
        "        k.launch(s, grid: [1], block: [1])(y, n);\n"
        "        s.sync();\n"
        "        // y dropped here by RAII — OK, borrow released at sync\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.M");
    EXPECT_EQ(codegenErrorId(compiler), "");
}
