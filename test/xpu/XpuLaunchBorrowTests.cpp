//
// CajetaXPU §3.5 / §11 — launch borrow scope checking.
//
// A `kernel.launch(...)(args)` borrows each KernelBuffer argument for the duration
// of the asynchronous launch; the borrow is released at the next
// KernelStream.sync() / Event.waitHost(). Freeing a buffer that a launch still
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
#include "KernelLoweringProbe.h"

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

using cajeta::xpu::probe::compileForInspection;


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
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n";

} // namespace

// Freeing a launch-borrowed buffer before syncing is XPU-K02.

// Syncing the stream releases the launch borrow, so the later free is fine.

// A free with no launch outstanding is unaffected by the check.

// Letting a launch-borrowed OWNED buffer leave scope before syncing is XPU-K02
// — the implicit-drop counterpart to freeBeforeSyncRejected. With KernelBuffer<T>'s
// RAII destructor, the scope-exit drop would free device memory an in-flight
// kernel still references.
TEST(XpuLaunchBorrowTests, dropBeforeSyncRejected) {
    std::string src = std::string(kHeader) +
        "public class M {\n"
        "    @Kernel public static void k(KernelBuffer<float32> y, uint32 n) { }\n"
        "    public static void run(KernelStream s, uint32 n) {\n"
        "        KernelBuffer<float32> y = heap KernelBuffer<float32>(n);\n"
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
        "    @Kernel public static void k(KernelBuffer<float32> y, uint32 n) { }\n"
        "    public static void run(KernelStream s, uint32 n) {\n"
        "        KernelBuffer<float32> y = heap KernelBuffer<float32>(n);\n"
        "        k.launch(s, grid: [1], block: [1])(y, n);\n"
        "        s.sync();\n"
        "        // y dropped here by RAII — OK, borrow released at sync\n"
        "    }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.M");
    EXPECT_EQ(codegenErrorId(compiler), "");
}
