//
// The codegen tuning of a lowered cpu kernel's per-block wrapper.
//
// The cpu backend does not codegen its kernels through its own TargetMachine:
// CpuRegistration links each lowered kernel into the HOST module and builds
// the per-block wrapper there, so the wrapper is compiled by the host
// program's TargetMachine, at the host program's codegen level, with the
// host CPU's scheduling model. On a Zen host that model turns LLVM's post-RA
// list scheduler on, and that scheduler is quadratic in basic-block size.
// A fission wrapper after unrolling IS one enormous basic block: the
// cajeta-llm test binary took 57 minutes to compile on Phoenix (znver2),
// with both sampled stacks in PostRAScheduler's buildSchedGraph
// (xpu-kernel-adaptor 6.4.12).
//
// The lever is per function, not per program. The wrapper carries a
// `tune-cpu` of generic, so ITS subtarget answers no post-RA scheduling and
// takes the tuning clang gives every program it compiles without -mtune,
// while the host program keeps the host CPU's tuning. The ISA is untouched:
// the wrapper carries no target-cpu and no target-features of its own, so it
// inherits the host machine's SIMD and the vectorizer still sees the host.
// CAJETA_XPU_CPU_TUNE names another tune target, and `host` keeps the host's
// (the A/B knob the plan's runtime measurement uses).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/cpu/CpuRegistration.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"

#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

#include "KernelLoweringProbe.h"
#include "PortableEnv.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using cajeta::Compiler;
using cajeta::xpu::probe::compileForInspection;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void tuneProbe(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                                 float32 a) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        y[i] = a * x[i] + y[i];\n"
    "    }\n"
    "}\n";

// A barrier kernel takes the FISSION path: its body is cloned into the wrapper
// region by region, and the clone replaces the wrapper's function attributes
// with the kernel's. The tune set at creation was lost there, and the
// kernel's `alwaysinline` landed on the wrapper, which the always-inliner
// then copied whole into the launch thunk (the cajeta-llm WmmaKernel module,
// 2026-09-29: four 230k to 275k instruction wrappers, each compiled twice).
const char* kBarrierSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Barrier;\n"
    "public class M {\n"
    "    @Kernel\n"
    "    public static void tuneStaged(KernelBuffer<uint32> a, KernelBuffer<uint32> b) {\n"
    "        uint32 t = KernelThread.globalIdX();\n"
    "        a[t] = t;\n"
    "        Barrier.workgroup();\n"
    "        b[t] = t + 100;\n"
    "    }\n"
    "}\n";

cajeta::MethodPtr findMethod(const cajeta::CajetaClassPtr& klass,
                             const std::string& name) {
    for (auto& [k, m] : klass->getMethods())
        if (m && m->getName() == name) return m;
    return nullptr;
}

// A host TargetMachine for a CPU whose scheduling model turns the post-RA
// scheduler ON: the host program's machine on Phoenix, and the one the
// measurement was taken on. Null off x86, where the test has nothing to say.
std::unique_ptr<llvm::TargetMachine> zenTargetMachine() {
    llvm::Triple triple(llvm::sys::getProcessTriple());
    if (!triple.isX86()) return nullptr;
    llvm::InitializeNativeTarget();
    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(triple, error);
    if (!target) return nullptr;
    llvm::TargetOptions opt;
    return std::unique_ptr<llvm::TargetMachine>(target->createTargetMachine(
        triple, "znver2", "", opt, llvm::Reloc::PIC_));
}

struct Lowered {
    std::unique_ptr<llvm::LLVMContext> ctx;
    std::unique_ptr<llvm::Module> host;
    llvm::Function* wrapper = nullptr;
    llvm::Function* hostFn = nullptr;
};

// Lower the probe kernel through the real registration path into a host
// module that also holds an ordinary host function.
Lowered lower(Compiler& compiler, llvm::TargetMachine& tm,
              const char* source = kSource, const char* kernel = "tuneProbe") {
    Lowered out;
    auto module = compileForInspection(compiler, source);
    auto k = findMethod(module->getStructures()["test.M"], kernel);
    if (!k) return out;
    out.ctx = std::make_unique<llvm::LLVMContext>();
    out.host = std::make_unique<llvm::Module>("xpu_cpu_tune", *out.ctx);
    out.host->setTargetTriple(tm.getTargetTriple());
    out.host->setDataLayout(tm.createDataLayout());
    std::vector<cajeta::MethodPtr> kernels{k};
    if (cajeta::xpu::cpu::emitKernelRegistration(kernels, *out.host) != 1)
        return out;
    for (llvm::Function& f : *out.host)
        if (f.getName().starts_with("__cajeta_xpu_cpu_block.")) out.wrapper = &f;
    out.hostFn = llvm::Function::Create(
        llvm::FunctionType::get(llvm::Type::getVoidTy(*out.ctx), false),
        llvm::GlobalValue::ExternalLinkage, "host_fn", *out.host);
    return out;
}

bool postRAScheduling(llvm::TargetMachine& tm, llvm::Function& f) {
    const llvm::TargetSubtargetInfo* st = tm.getSubtargetImpl(f);
    return st != nullptr && st->enablePostRAScheduler();
}

struct ScopedEnv {
    const char* name;
    explicit ScopedEnv(const char* n, const char* v) : name(n) { setenv(n, v, 1); }
    ~ScopedEnv() { unsetenv(name); }
};

} // namespace

TEST(XpuCpuKernelTuneTests, kernelWrappersDropThePostRASchedulerTheHostKeeps) {
    auto tm = zenTargetMachine();
    if (!tm) GTEST_SKIP() << "x86 only: the post-RA model in question is Zen's";
    Compiler compiler;
    Lowered l = lower(compiler, *tm);
    ASSERT_NE(l.wrapper, nullptr) << "no per-block wrapper was emitted";

    // The host program keeps the host CPU's model, which turns the pass on:
    // this is the half that says the lever is needed at all.
    EXPECT_TRUE(postRAScheduling(*tm, *l.hostFn));
    // The wrapper's subtarget does not run it.
    EXPECT_FALSE(postRAScheduling(*tm, *l.wrapper));
    EXPECT_EQ(l.wrapper->getFnAttribute("tune-cpu").getValueAsString(), "generic");
    // Tuning only: the ISA stays whatever the host machine says.
    EXPECT_FALSE(l.wrapper->hasFnAttribute("target-cpu"));
    EXPECT_FALSE(l.wrapper->hasFnAttribute("target-features"));
}

TEST(XpuCpuKernelTuneTests, hostKeepsTheHostTuningOnTheWrappersForTheMeasurement) {
    auto tm = zenTargetMachine();
    if (!tm) GTEST_SKIP() << "x86 only: the post-RA model in question is Zen's";
    ScopedEnv env("CAJETA_XPU_CPU_TUNE", "host");
    Compiler compiler;
    Lowered l = lower(compiler, *tm);
    ASSERT_NE(l.wrapper, nullptr) << "no per-block wrapper was emitted";

    EXPECT_FALSE(l.wrapper->hasFnAttribute("tune-cpu"));
    EXPECT_TRUE(postRAScheduling(*tm, *l.wrapper));
}

TEST(XpuCpuKernelTuneTests, anotherTuneTargetIsNamedByTheSameKnob) {
    auto tm = zenTargetMachine();
    if (!tm) GTEST_SKIP() << "x86 only: the post-RA model in question is Zen's";
    ScopedEnv env("CAJETA_XPU_CPU_TUNE", "haswell");
    Compiler compiler;
    Lowered l = lower(compiler, *tm);
    ASSERT_NE(l.wrapper, nullptr) << "no per-block wrapper was emitted";

    EXPECT_EQ(l.wrapper->getFnAttribute("tune-cpu").getValueAsString(), "haswell");
    EXPECT_FALSE(postRAScheduling(*tm, *l.wrapper));
}

TEST(XpuCpuKernelTuneTests, aFissionWrapperKeepsTheTuneAndIsNotAlwaysInline) {
    auto tm = zenTargetMachine();
    if (!tm) GTEST_SKIP() << "x86 only: the post-RA model in question is Zen's";
    Compiler compiler;
    Lowered l = lower(compiler, *tm, kBarrierSource, "tuneStaged");
    ASSERT_NE(l.wrapper, nullptr) << "no per-block wrapper was emitted";

    EXPECT_EQ(l.wrapper->getFnAttribute("tune-cpu").getValueAsString(), "generic");
    EXPECT_FALSE(postRAScheduling(*tm, *l.wrapper));
    // The kernel's alwaysinline is for inlining the kernel INTO the wrapper;
    // on the wrapper it would copy the whole body into the launch thunk.
    EXPECT_FALSE(l.wrapper->hasFnAttribute(llvm::Attribute::AlwaysInline));
    // The kernel's other attributes the clone carried over still ride it.
    EXPECT_TRUE(l.wrapper->hasFnAttribute("cajeta-cpu-kernel"));
}
