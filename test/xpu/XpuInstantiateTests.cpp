//
// Explicit instantiation of a class template (xpu-kernel-independence plan
// Unit 6, spec §3.4 and §7.3; moved from xpu-tile-shape-selection Unit 4).
//
// A library builds the instantiations it needs ahead of time. A class
// template lists them, `@Instantiate({Tile<4>, Tile<7>})`, and every listed
// instantiation is lowered at build even when no code names it, so an AOT
// executable carries all of them. The list is capped, at 8 unless the class
// declares `max`, so the compile cost is known up front. A template states
// what its body can serve with `@Requires(<constant expression>)` over its
// values, and an instantiation that fails it is refused by name, listed or
// not, instead of being miscompiled.
//
#include "gtest/gtest.h"

#include "KernelLoweringProbe.h"

#include "cajeta/compile/CajetaModule.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/error/Exception.h"
#include "cajeta/method/Method.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/ConstExpr.h"
#include "cajeta/xpu/core/KernelManifest.h"
#include "cajeta/xpu/nvidia/NvptxRegistration.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <set>
#include <string>
#include <vector>

namespace {

// No code names Tile: only the list makes the instantiations.
const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;
@Instantiate({Tile<16>, Tile<32>, Tile<64>})
@Requires(N % 16 == 0)
public class Tile<uint32 N> {
    public Tile() { return; }
    @Kernel
    public static void fill(KernelBuffer<uint32> out) {
        out[KernelThread.globalIdX()] = N;
    }
}
public class M {
    public static int32 run() { return 0; }
}
)CJ";

std::set<std::string> tileInstances(const cajeta::CajetaModulePtr& module,
                                    std::vector<cajeta::MethodPtr>* kernels) {
    std::set<std::string> out;
    for (auto& [canon, klass] : module->getStructures()) {
        if (canon.rfind("test.Tile<", 0) != 0 || !klass) continue;
        if (!klass->isInstantiation()) continue;
        out.insert(canon);
        for (auto& [k, m] : klass->getMethods())
            if (m && m->getName() == "fill" && kernels) kernels->push_back(m);
    }
    return out;
}

// The compile's error, or "" when it compiled.
std::string compileError(const std::string& source) {
    cajeta::Compiler compiler;
    try {
        auto module = cajeta::xpu::probe::compileForInspection(compiler, source);
        return module ? "" : "did not compile";
    } catch (cajeta::Exception& e) {
        return e.getMessage();
    } catch (const char* e) {
        return e;
    } catch (std::exception& e) {
        return e.what();
    }
}

std::string replaced(std::string s, const std::string& from, const std::string& to) {
    s.replace(s.find(from), from.size(), to);
    return s;
}

} // namespace

// 4.6.1.1: every listed instantiation is built with no reference site, and
// each carries its kernel into the manifest set.
TEST(XpuInstantiate, everyListedInstantiationLowersAtBuild) {
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, kSource);
    ASSERT_NE(module, nullptr);
    std::vector<cajeta::MethodPtr> kernels;
    const auto inst = tileInstances(module, &kernels);
    EXPECT_EQ(inst, (std::set<std::string>{"test.Tile<16>", "test.Tile<32>",
                                           "test.Tile<64>"}));
    ASSERT_EQ(kernels.size(), 3u);

    llvm::LLVMContext ctx;
    llvm::Module host("xpu_instantiate_host", ctx);
    std::vector<cajeta::xpu::KernelManifest> out;
    testing::internal::CaptureStderr();
    cajeta::xpu::nvidia::emitKernelRegistration(kernels, host, "sm_89", &out);
    std::string err = testing::internal::GetCapturedStderr();
    if (out.empty()) {
        GTEST_SKIP() << "no cubin assembled on this box (ptxas absent or too "
                        "old), so there is no manifest to inspect: " << err;
    }
    std::set<std::string> names;
    for (auto& m : out) names.insert(m.kernel);
    EXPECT_EQ(names, (std::set<std::string>{"test.Tile<16>.fill", "test.Tile<32>.fill",
                                            "test.Tile<64>.fill"}))
        << err;
}

// 4.6.1.1: a listed instantiation that code also names is built once.
TEST(XpuInstantiate, aListedInstantiationThatCodeNamesIsBuiltOnce) {
    const std::string src = replaced(kSource, "public static int32 run() { return 0; }",
        "public static int32 run() { Tile<32> t #= heap Tile<32>(); return 0; }");
    cajeta::Compiler compiler;
    auto module = cajeta::xpu::probe::compileForInspection(compiler, src);
    ASSERT_NE(module, nullptr);
    std::vector<cajeta::MethodPtr> kernels;
    EXPECT_EQ(tileInstances(module, &kernels).size(), 3u);
    EXPECT_EQ(kernels.size(), 3u);
}

// 4.6.1.2: a list over the cap is refused, naming the class, the count and
// the cap.
TEST(XpuInstantiate, aListOverTheDeclaredCapIsRefusedNamingTheClass) {
    const std::string src = replaced(kSource,
        "@Instantiate({Tile<16>, Tile<32>, Tile<64>})",
        "@Instantiate(value = {Tile<16>, Tile<32>, Tile<64>}, max = 2)");
    const std::string why = compileError(src);
    ASSERT_FALSE(why.empty()) << "three instantiations under max = 2 compiled";
    EXPECT_NE(why.find("test.Tile"), std::string::npos) << why;
    EXPECT_NE(why.find("@Instantiate"), std::string::npos) << why;
    EXPECT_NE(why.find("3"), std::string::npos) << why;
    EXPECT_NE(why.find("max = 2"), std::string::npos) << why;
}

// 4.6.1.2: the cap without `max` is 8.
TEST(XpuInstantiate, theDefaultCapIsEight) {
    std::string list;
    for (int n = 1; n <= 9; ++n) list += (n > 1 ? ", " : "") + std::string("Tile<")
                                         + std::to_string(16 * n) + ">";
    const std::string nine = replaced(kSource,
        "{Tile<16>, Tile<32>, Tile<64>}", "{" + list + "}");
    const std::string why = compileError(nine);
    ASSERT_FALSE(why.empty()) << "nine instantiations compiled under the default cap";
    EXPECT_NE(why.find("max = 8"), std::string::npos) << why;

    const std::string eight = replaced(nine, ", Tile<144>", "");
    EXPECT_EQ(compileError(eight), "");
}

// 4.6.1.3: a listed instantiation the body cannot serve is refused by name,
// naming the instantiation and the requirement it fails.
TEST(XpuInstantiate, aListedShapeTheBodyCannotServeIsRefusedByName) {
    const std::string src = replaced(kSource, "Tile<32>, Tile<64>", "Tile<24>, Tile<64>");
    const std::string why = compileError(src);
    ASSERT_FALSE(why.empty()) << "Tile<24> compiled under @Requires(N % 16 == 0)";
    EXPECT_NE(why.find("Tile<24>"), std::string::npos) << why;
    EXPECT_NE(why.find("N % 16 == 0"), std::string::npos) << why;
}

// 4.6.1.3: the requirement holds at every instantiation, listed or not.
TEST(XpuInstantiate, aRequirementHoldsAtAnInstantiationCodeNames) {
    const std::string src = replaced(kSource, "public static int32 run() { return 0; }",
        "public static int32 run() { Tile<40> t #= heap Tile<40>(); return 0; }");
    const std::string why = compileError(src);
    ASSERT_FALSE(why.empty()) << "Tile<40> compiled under @Requires(N % 16 == 0)";
    EXPECT_NE(why.find("Tile<40>"), std::string::npos) << why;
}

// A listed entry that is not an instantiation of this class is refused.
TEST(XpuInstantiate, anEntryOfAnotherClassIsRefused) {
    const std::string src = replaced(kSource, "Tile<64>}", "Other<64>}");
    const std::string why = compileError(src);
    ASSERT_FALSE(why.empty());
    EXPECT_NE(why.find("Other<64>"), std::string::npos) << why;
}

// The evaluator behind @Requires: comparisons, equality and logic at the
// language's precedence, each yielding 1 or 0, beside shifts it must not
// confuse them with.
TEST(XpuInstantiate, requirementsEvaluateComparisonsAndLogic) {
    auto val = [](const std::string& text) {
        auto r = cajeta::evalConstExpr(text, [](const std::string& n) -> std::optional<int64_t> {
            if (n == "WM") return 48;
            if (n == "WN") return 64;
            return std::nullopt;
        });
        EXPECT_TRUE(r.ok) << text << ": " << r.error;
        return r.value;
    };
    EXPECT_EQ(val("WM % 16 == 0 && WN % 16 == 0"), 1);
    EXPECT_EQ(val("WM % 32 == 0 || WN % 32 == 0"), 1);
    EXPECT_EQ(val("WM % 32 == 0 && WN % 32 == 0"), 0);
    EXPECT_EQ(val("!(WM > WN)"), 1);
    EXPECT_EQ(val("WM <= 48 && WM >= 48 && WM < 49 && WM != 47"), 1);
    EXPECT_EQ(val("1 << 4 < 17"), 1) << "a shift binds tighter than a comparison";
    EXPECT_EQ(val("(WM >> 4) == 3"), 1);
    EXPECT_EQ(val("6 & 3 == 2"), 0) << "== binds tighter than &, as in Java";
    auto bad = cajeta::evalConstExpr("WM % 16 == lanes", [](const std::string& n)
        -> std::optional<int64_t> { if (n == "WM") return 48; return std::nullopt; });
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(bad.unbound, "lanes");
}
