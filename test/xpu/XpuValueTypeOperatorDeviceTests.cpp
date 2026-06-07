//
// S8 of the value-type operator-overloading mechanism
// (plans/value-type-overloading-plan.md): a @ValueType's static @Device
// operator dispatches INSIDE a kernel. `a == b` on value-type operands routes
// through the SAME S6 dispatch/derivation policy the host uses
// (opdispatch::dispatchBinaryOperator) — only the resolve+invoke/negate
// callbacks are device-specific: resolve the operator, require @Device (pure),
// lower it as an alwaysinline aggregate-param helper, emit the call. The derived
// `a != b` (¬(a == b)) lowers too. v1 covers scalar-returning operators (==, <);
// a value-type-RETURNING device operator (aggregate construction in a kernel) is
// the next increment. Mirrors XpuVectorDeviceTests (CPU lowering, IR assertions).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/vulkan/SpirvBackend.h"
#include "cajeta/xpu/vulkan/SpirvKernelLowering.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModulePtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_xpu_vtop_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_vtop_arch_" + std::to_string(rng()));
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

std::string printModule(llvm::Module& m) {
    std::string s;
    llvm::raw_string_ostream os(s);
    m.print(os, nullptr);
    return os.str();
}

// Vec2 with a pure @Device operator== (scalar boolean return).
const char* kVec2Eq =
    "@ValueType public final class Vec2 {\n"
    "    public float32 x;\n"
    "    public float32 y;\n"
    "    public Vec2(float32 x, float32 y) { this.x = x; this.y = y; }\n"
    "    @Device public static boolean operator== (Vec2 a, Vec2 b) {\n"
    "        return a.x == b.x && a.y == b.y;\n"
    "    }\n"
    "}\n";

// Vec2 with a pure @Device operator+ that RETURNS a Vec2 (an aggregate). The
// operator constructs its result by value inside the kernel — the device
// lowerer builds the {x,y} struct via insertvalue and returns it by value.
const char* kVec2Add =
    "@ValueType public final class Vec2 {\n"
    "    public float32 x;\n"
    "    public float32 y;\n"
    "    public Vec2(float32 x, float32 y) { this.x = x; this.y = y; }\n"
    "    @Device public static Vec2 operator+ (Vec2 a, Vec2 b) {\n"
    "        return heap Vec2(a.x + b.x, a.y + b.y);\n"
    "    }\n"
    "}\n";

std::string lowerKernelIr(const std::string& src, const std::string& cls,
                          const std::string& kernel) {
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()[cls], kernel);
    EXPECT_NE(k, nullptr);
    if (!k) return {};
    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    EXPECT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_vtop", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    // A throw (unsupported) would fail the test — a clean lowering of a kernel
    // that dispatches a @Device value-type operator IS the proof.
    EXPECT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);
    return printModule(host);
}

} // namespace

// `a == b` in a kernel dispatches to Vec2's @Device operator==: the device
// operator function is emitted and called, and its body's field comparison
// (fcmp) is present.
TEST(XpuValueTypeOperatorDeviceTests, deviceEqualityOperatorDispatches) {
    std::string src =
        std::string("package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n")
        + kVec2Eq +
        "public class M {\n"
        "    @Kernel\n"
        "    public static void eqk(Vec2 a, Vec2 b, Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            float32 r = 0.0f;\n"
        "            if (a == b) { r = 1.0f; }\n"
        "            out[i] = r + (float32) i;\n"
        "        }\n"
        "    }\n"
        "}\n";
    std::string ir = lowerKernelIr(src, "test.M", "eqk");
    ASSERT_FALSE(ir.empty());
    EXPECT_NE(ir.find("operator=="), std::string::npos)
        << "expected the @Device operator== to be emitted/dispatched\n" << ir;
    EXPECT_NE(ir.find("fcmp"), std::string::npos)
        << "expected the operator body's field comparison\n" << ir;
}

// `a != b` derives from operator== via the shared S6 policy on device too.
TEST(XpuValueTypeOperatorDeviceTests, deviceInequalityDerivesFromEquality) {
    std::string src =
        std::string("package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n")
        + kVec2Eq +
        "public class M {\n"
        "    @Kernel\n"
        "    public static void nek(Vec2 a, Vec2 b, Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            float32 r = 0.0f;\n"
        "            if (a != b) { r = 1.0f; }\n"
        "            out[i] = r + (float32) i;\n"
        "        }\n"
        "    }\n"
        "}\n";
    std::string ir = lowerKernelIr(src, "test.M", "nek");
    ASSERT_FALSE(ir.empty());
    // Derived: operator== is still the dispatched primitive, negated.
    EXPECT_NE(ir.find("operator=="), std::string::npos)
        << "a != b should derive from the @Device operator==\n" << ir;
}

// A value-type-RETURNING @Device operator dispatches inside a kernel: `a + b`
// routes to Vec2's @Device operator+, which constructs a Vec2 result by value
// (insertvalue chain into the device struct) and returns it. The kernel binds
// that aggregate to a value-type local and reads its fields — proving device
// aggregate construction, aggregate return, and value-type locals all lower.
TEST(XpuValueTypeOperatorDeviceTests, deviceAggregateReturningOperatorDispatches) {
    std::string src =
        std::string("package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n")
        + kVec2Add +
        "public class M {\n"
        "    @Kernel\n"
        "    public static void addk(Vec2 a, Vec2 b, Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vec2 c = a + b;\n"
        "            out[i] = c.x + c.y + (float32) i;\n"
        "        }\n"
        "    }\n"
        "}\n";
    std::string ir = lowerKernelIr(src, "test.M", "addk");
    ASSERT_FALSE(ir.empty());
    // The @Device operator+ is emitted and dispatched.
    EXPECT_NE(ir.find("operator+"), std::string::npos)
        << "expected the @Device operator+ to be emitted/dispatched\n" << ir;
    // Its body builds the Vec2 result by value (insertvalue) and the kernel
    // reads the returned aggregate's fields (extractvalue).
    EXPECT_NE(ir.find("insertvalue"), std::string::npos)
        << "expected aggregate construction of the Vec2 result\n" << ir;
    EXPECT_NE(ir.find("extractvalue"), std::string::npos)
        << "expected the returned aggregate's fields to be read\n" << ir;
}

// The value-type operator path is SPIR-V-valid. A kernel that constructs Vec2s
// from scalars, dispatches the @Device operator+ (aggregate construction +
// return), binds the result to a value-type local, and reads its fields lowers
// to a Vulkan module that spirv-val accepts. Value types stay register-resident
// SSA aggregates (OpCompositeInsert/Extract), with NO pointer to an aggregate in
// Function storage — the property that keeps them legal under logical addressing.
// Built locally (not as struct params) so the kernel signature is a plain buffer
// + scalar; struct-by-value descriptor marshalling on Vulkan is separate. Gated
// on spirv-val like XpuWaveEmitTests — absent tool => skipped, not failed.
TEST(XpuValueTypeOperatorDeviceTests, deviceOperatorKernelValidatesAsSpirv) {
    std::string src =
        std::string("package test;\n"
        "import cajeta.xpu.core.Buffer;\n"
        "import cajeta.xpu.core.Thread;\n")
        + kVec2Add +
        "public class M {\n"
        "    @Kernel\n"
        "    public static void addk(Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) {\n"
        "            Vec2 a = heap Vec2((float32) i, 1.0f);\n"
        "            Vec2 b = heap Vec2(2.0f, (float32) i);\n"
        "            Vec2 c = a + b;\n"
        "            out[i] = c.x + c.y;\n"
        "        }\n"
        "    }\n"
        "}\n";

    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], "addk");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::vulkan::createSpirvTargetMachine("vulkan1.3");
    ASSERT_NE(tm, nullptr);

    // IR pass: confirm the value-type operator path lowered (aggregate ops).
    llvm::LLVMContext irCtx;
    llvm::Module irMod("xpu_vtop_spirv_ir", irCtx);
    cajeta::xpu::vulkan::configureDeviceModule(irMod, *tm);
    ASSERT_NE(cajeta::xpu::vulkan::lowerKernel(k, irMod), nullptr);
    std::string ir = printModule(irMod);
    EXPECT_NE(ir.find("insertvalue"), std::string::npos) << ir;
    EXPECT_NE(ir.find("extractvalue"), std::string::npos) << ir;

    // Emit pass (mutates the module) then spirv-val.
    llvm::LLVMContext binCtx;
    llvm::Module binMod("xpu_vtop_spirv_bin", binCtx);
    cajeta::xpu::vulkan::configureDeviceModule(binMod, *tm);
    cajeta::xpu::vulkan::lowerKernel(k, binMod);
    std::vector<uint8_t> spirv = cajeta::xpu::vulkan::emitSpirv(binMod, *tm);
    ASSERT_FALSE(spirv.empty());

    auto tool = llvm::sys::findProgramByName("spirv-val");
    if (!tool) { GTEST_SUCCEED() << "spirv-val absent; skipped validation"; return; }
    static std::mt19937_64 rng(std::random_device{}());
    auto path = std::filesystem::temp_directory_path()
              / ("cajeta_vtop_" + std::to_string(rng()) + ".spv");
    { std::ofstream o(path, std::ios::binary);
      o.write(reinterpret_cast<const char*>(spirv.data()),
              (std::streamsize) spirv.size()); }
    std::string fileStr = path.string();
    llvm::StringRef env = "--target-env", ver = "vulkan1.3", file = fileStr;
    llvm::SmallVector<llvm::StringRef, 4> args = {*tool, env, ver, file};
    int rc = llvm::sys::ExecuteAndWait(*tool, args);
    std::filesystem::remove(path);
    EXPECT_EQ(rc, 0) << "spirv-val rejected the value-type-operator module";
}
