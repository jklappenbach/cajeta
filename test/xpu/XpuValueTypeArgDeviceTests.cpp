//
// S5 of the value-type operator-overloading mechanism
// (plans/value-type-overloading-plan.md): @ValueType PODs marshal by value as
// @Kernel arguments, and a struct that CONTAINS a value-type field does too
// (recursively). The device lowerer reads their fields — a flat `param.field`
// and a nested `param.vfield.subfield` — as (multi-index) extractvalue from the
// param's SSA aggregate, the same shape the existing primitive-POD path uses.
// No operators here (S8 covers device operators); this is pure marshalling +
// field reads. Mirrors XpuVectorDeviceTests.lowersToVectorIr (CPU lowering,
// IR assertions — the host struct-arg execution ABI is out of scope).
//

#include "gtest/gtest.h"

#include "cajeta/xpu/cpu/CpuKernelLowering.h"
#include "cajeta/xpu/cpu/CpuBackend.h"
#include "cajeta/xpu/core/KernelArgTrait.h"

#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/method/Method.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
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
              / ("cajeta_xpu_vtarg_" + std::to_string(rng()));
    std::filesystem::create_directories(base / "test");
    std::ofstream(base / "test" / "M.cajeta") << source;
    auto archive = std::filesystem::temp_directory_path()
                 / ("cajeta_xpu_vtarg_arch_" + std::to_string(rng()));
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

const char* kVec2 =
    "@ValueType public final class Vec2 {\n"
    "    public float32 x;\n"
    "    public float32 y;\n"
    "    public Vec2(float32 x, float32 y) { this.x = x; this.y = y; }\n"
    "}\n";

} // namespace

// A @ValueType passed by value as a kernel arg is admissible and lowers: the
// kernel reads its fields (v.x, v.y) as extractvalue from the param aggregate.
TEST(XpuValueTypeArgDeviceTests, flatValueTypeArgFieldReadLowers) {
    std::string src =
        std::string("package test;\n"
        "import cajeta.gpu.Buffer;\n"
        "import cajeta.gpu.Thread;\n")
        + kVec2 +
        "public class M {\n"
        "    @Kernel\n"
        "    public static void readvt(Vec2 v, Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { out[i] = v.x + v.y + (float32) i; }\n"
        "    }\n"
        "}\n";

    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.M"], "readvt");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_vtarg_flat", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    // A throw here (unsupported field read) would fail the test — successful
    // lowering of a field-reading kernel IS the proof the marshalling worked.
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);

    std::string ir = printModule(host);
    EXPECT_NE(ir.find("extractvalue"), std::string::npos) << ir;
}

// A struct that CONTAINS @ValueType fields marshals by value too, and a nested
// read (b.lo.x, b.hi.y) lowers to a two-level extractvalue.
TEST(XpuValueTypeArgDeviceTests, nestedValueTypeFieldReadLowers) {
    std::string src =
        std::string("package test;\n"
        "import cajeta.gpu.Buffer;\n"
        "import cajeta.gpu.Thread;\n")
        + kVec2 +
        "@ValueType public final class Box {\n"
        "    public Vec2 lo;\n"
        "    public Vec2 hi;\n"
        "    public Box(Vec2 lo, Vec2 hi) { this.lo = lo; this.hi = hi; }\n"
        "}\n"
        "public class MN {\n"
        "    @Kernel\n"
        "    public static void readnested(Box b, Buffer<float32> out, uint32 n) {\n"
        "        uint32 i = Thread.globalIdX();\n"
        "        if (i < n) { out[i] = b.lo.x + b.hi.y + (float32) i; }\n"
        "    }\n"
        "}\n";

    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto k = findMethod(module->getStructures()["test.MN"], "readnested");
    ASSERT_NE(k, nullptr);

    auto tm = cajeta::xpu::cpu::createCpuTargetMachine();
    ASSERT_NE(tm, nullptr) << "host target not registered";
    llvm::LLVMContext ctx;
    llvm::Module host("xpu_vtarg_nested", ctx);
    cajeta::xpu::cpu::configureHostModule(host, *tm);
    ASSERT_NE(cajeta::xpu::cpu::lowerKernel(k, host), nullptr);

    std::string ir = printModule(host);
    EXPECT_NE(ir.find("extractvalue"), std::string::npos) << ir;
}

// Admissibility: a value type and a value-type-containing struct both satisfy
// the kernel-arg POD predicate directly.
TEST(XpuValueTypeArgDeviceTests, valueTypeAndContainerArePodStructs) {
    std::string src =
        std::string("package test;\n") + kVec2 +
        "@ValueType public final class Box {\n"
        "    public Vec2 lo;\n"
        "    public Vec2 hi;\n"
        "    public Box(Vec2 lo, Vec2 hi) { this.lo = lo; this.hi = hi; }\n"
        "}\n"
        "public class K {\n"
        "    @Kernel\n"
        "    public static void run(Vec2 v, Box b) { }\n"
        "}\n";
    Compiler compiler;
    auto module = compileForInspection(compiler, src);
    auto vec2 = std::dynamic_pointer_cast<cajeta::CajetaClass>(
        module->getStructures()["test.Vec2"]);
    auto box = std::dynamic_pointer_cast<cajeta::CajetaClass>(
        module->getStructures()["test.Box"]);
    ASSERT_NE(vec2, nullptr);
    ASSERT_NE(box, nullptr);
    EXPECT_TRUE(cajeta::xpu::isPodStructType(vec2));
    EXPECT_TRUE(cajeta::xpu::isPodStructType(box));
}
