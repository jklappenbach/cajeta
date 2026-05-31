//
// CajetaXPU CPU backend (Increment 3) — CpuDriver: launch a registered kernel
// BY NAME over a grid through the uniform launcher-thunk ABI.
//
// Unlike XpuCpuExecTests (which JIT-looks-up the kernel and hand-builds the grid
// loop with a statically-typed function pointer), this drives the REAL path: the
// kernel is lowered + registered via cpu::emitKernelRegistration (kernel +
// __cajeta_xpu_cpu_launch.<name> thunk + ctor), the thunk is registered in the
// runtime registry, and CpuDriver resolves it by name and runs the grid→threads
// loop — calling the thunk per work-item with the kernelParams argv + that
// work-item's coordinate vector. This is the by-name launch the Inc-4 dispatcher
// is built on. Serial; GPU-free.
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuDriver.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"
#include "cajeta/xpu/cpu/CpuBackend.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorSymbolDef.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Error.h"
#include "llvm/Target/TargetMachine.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

// The runtime CPU kernel registry — same symbols CpuDriver / the registration
// ctor speak, linked into the test binary via cajeta_lib.
extern "C" void __cajeta_xpu_register_cpu_kernel(const char* name, void* fn);

using cajeta::Compiler;
using cajeta::CajetaModulePtr;
using cajeta::xpu::cpu::CpuDriver;
using cajeta::xpu::cpu::CpuLaunchFn;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_cpudrv_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_cpudrv_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto full = base / "test" / "M.cajeta";
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// Lower + register `kernelName` from `source` into a fresh host module, JIT it,
// and run its module initializers so the REAL registration ctor registers the
// launcher thunk in the runtime registry under `kernelName`. Returns the live
// LLJIT (keep it in scope while launching — the registered pointer is JIT'd
// code). On any failure, returns nullptr and sets `failure`.
std::unique_ptr<llvm::orc::LLJIT> registerKernel(Compiler& compiler,
                                                 const std::string& source,
                                                 const std::string& kernelName,
                                                 std::string& failure) {
    auto module = compileForInspection(compiler, source);
    auto k = findMethod(module->getStructures()["test.M"], kernelName);
    if (!k) { failure = "kernel not found: " + kernelName; return nullptr; }

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    if (!tm) { failure = "host target not registered"; return nullptr; }

    auto ctx = std::make_unique<llvm::LLVMContext>();
    auto host = std::make_unique<llvm::Module>("xpu_cpu_driver", *ctx);
    cajeta::xpu::cpu::configureHostModule(*host, *tm);

    // The real registration path: lowers the kernel + the launcher thunk +
    // the registration ctor into `host`.
    std::vector<cajeta::MethodPtr> kernels{k};
    if (cajeta::xpu::cpu::emitKernelRegistration(kernels, *host) != 1) {
        failure = "emitKernelRegistration did not embed the kernel";
        return nullptr;
    }

    auto jitOrErr = llvm::orc::LLJITBuilder().create();
    if (!jitOrErr) { failure = llvm::toString(jitOrErr.takeError()); return nullptr; }
    auto jit = std::move(*jitOrErr);
    auto& JD = jit->getMainJITDylib();

    // The registration ctor calls __cajeta_xpu_register_cpu_kernel, which lives
    // in the test binary (cajeta_lib) but isn't a dynamically-exported symbol —
    // so map it into the JIT explicitly as an absolute symbol pointing at the
    // process function. The ctor then registers into the very registry CpuDriver
    // reads back.
    llvm::orc::MangleAndInterner mangle(jit->getExecutionSession(),
                                        jit->getDataLayout());
    llvm::orc::SymbolMap syms;
    syms[mangle("__cajeta_xpu_register_cpu_kernel")] = llvm::orc::ExecutorSymbolDef(
        llvm::orc::ExecutorAddr::fromPtr(&__cajeta_xpu_register_cpu_kernel),
        llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable);
    if (auto err = JD.define(llvm::orc::absoluteSymbols(std::move(syms)))) {
        failure = llvm::toString(std::move(err));
        return nullptr;
    }

    if (auto err = jit->addIRModule(
            llvm::orc::ThreadSafeModule(std::move(host), std::move(ctx)))) {
        failure = llvm::toString(std::move(err));
        return nullptr;
    }

    // Run llvm.global_ctors — the registration ctor fires and registers the
    // thunk by name.
    if (auto err = jit->initialize(JD)) {
        failure = llvm::toString(std::move(err));
        return nullptr;
    }
    return jit;
}

// SAXPY, 3 params (y, x, a) — the buffer + scalar unpack shape.
const char* kSaxpySource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
    "                             float32 a) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        y[i] = a * x[i] + y[i];\n"
    "    }\n"
    "}\n";

// SAXPY, 4 params (y, x, a, n) with an n-bounds guard — a DIFFERENT param shape
// (an extra uint32 scalar + an if), proving the FunctionType-driven unpacker is
// general, not SAXPY-3-shaped.
const char* kSaxpyGuardedSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void saxpy(Buffer<float32> y, Buffer<float32> x,\n"
    "                             float32 a, uint32 n) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        if (i < n) {\n"
    "            y[i] = a * x[i] + y[i];\n"
    "        }\n"
    "    }\n"
    "}\n";

// A uniquely-named kernel so this test's registry entry is created fresh by THIS
// test (not aliased to a "saxpy" some other test already registered+owns) — that
// is what makes the dangling-key regression below reproducible in isolation.
const char* kSaxpyTeardownSource =
    "package test;\n"
    "import cajeta.xpu.core.Buffer;\n"
    "import cajeta.xpu.core.Thread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void saxpy_teardown_probe(Buffer<float32> y,\n"
    "                                             Buffer<float32> x, float32 a) {\n"
    "        uint32 i = Thread.globalIdX();\n"
    "        y[i] = a * x[i] + y[i];\n"
    "    }\n"
    "}\n";

} // namespace

// CpuDriver resolves the kernel by name and runs it over a grid; globalId.x =
// ctaid*ntid + tid covers [0, N).
TEST(XpuCpuDriverTests, launchesSaxpyByNameOverAGrid) {
    Compiler compiler;
    std::string failure;
    auto jit = registerKernel(compiler, kSaxpySource, "saxpy", failure);
    ASSERT_NE(jit, nullptr) << failure;

    const unsigned B = 64, G = 4;
    const int N = static_cast<int>(B * G);
    const float a = 3.0f;
    std::vector<float> x(N), y(N), y0(N);
    for (int i = 0; i < N; ++i) {
        x[i] = static_cast<float>(i);
        y[i] = y0[i] = static_cast<float>(2 * i + 1);
    }

    // kernelParams (cuLaunch/hipModuleLaunch convention): argv[i] → &arg_i.
    float* yptr = y.data();
    float* xptr = x.data();
    float aval = a;
    void* argv[] = {&yptr, &xptr, &aval};

    CpuDriver d;
    ASSERT_TRUE(CpuDriver::available());
    ASSERT_TRUE(d.launch("saxpy", argv, /*gridX=*/G, /*blockX=*/B));

    for (int i = 0; i < N; ++i)
        EXPECT_FLOAT_EQ(y[i], a * x[i] + y0[i]) << "element " << i;
}

// A different param signature (extra uint32 + a bounds guard) drives the same
// unpacker: the launch must apply SAXPY where i < n and leave the tail untouched.
TEST(XpuCpuDriverTests, scalarUnpackingAcrossParamShapes) {
    Compiler compiler;
    std::string failure;
    auto jit = registerKernel(compiler, kSaxpyGuardedSource, "saxpy", failure);
    ASSERT_NE(jit, nullptr) << failure;

    const unsigned B = 64, G = 4;
    const int N = static_cast<int>(B * G);
    const std::uint32_t n = 200;          // guard cuts off before the grid end
    const float a = 2.5f;
    std::vector<float> x(N), y(N), y0(N);
    for (int i = 0; i < N; ++i) {
        x[i] = static_cast<float>(i);
        y[i] = y0[i] = static_cast<float>(i + 1);
    }

    float* yptr = y.data();
    float* xptr = x.data();
    float aval = a;
    std::uint32_t nval = n;
    void* argv[] = {&yptr, &xptr, &aval, &nval};

    CpuDriver d;
    ASSERT_TRUE(d.launch("saxpy", argv, /*gridX=*/G, /*blockX=*/B));

    for (int i = 0; i < N; ++i) {
        float expected = (static_cast<std::uint32_t>(i) < n)
                             ? a * x[i] + y0[i]
                             : y0[i];          // untouched past the guard
        EXPECT_FLOAT_EQ(y[i], expected) << "element " << i;
    }
}

// The precise "no such kernel" contract: launching an unregistered name fails
// rather than crashing.
TEST(XpuCpuDriverTests, lookupMissOnUnknownKernel) {
    CpuDriver d;
    void* argv[] = {};
    EXPECT_FALSE(d.launch("definitely_not_a_registered_kernel", argv,
                          /*gridX=*/1, /*blockX=*/1));
}

// Regression: the registry must OWN its keys. A registration ctor runs from JIT'd
// code whose module memory — including the `xpu.cpu.kname.<name>` string the ctor
// passes in — is freed when the JIT engine is torn down. If the registry kept the
// raw caller pointer, a later lookup's strcmp() would dereference that freed key
// and crash. Here we register a uniquely-named kernel, destroy its JIT, then do a
// lookup: it must be a clean miss, not a SIGSEGV. (Previously crashed.)
TEST(XpuCpuDriverTests, registryKeySurvivesCallerTeardown) {
    Compiler compiler;
    std::string failure;
    {
        auto jit = registerKernel(compiler, kSaxpyTeardownSource,
                                  "saxpy_teardown_probe", failure);
        ASSERT_NE(jit, nullptr) << failure;
    }   // JIT destroyed here — the memory holding the kname key is freed.

    CpuDriver d;
    void* argv[] = {};
    EXPECT_FALSE(d.launch("definitely_not_a_registered_kernel", argv,
                          /*gridX=*/1, /*blockX=*/1));
}
