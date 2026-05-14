//
// A2: @Aspect class registration during compile.
//
// A class annotated `@Aspect` is registered on the process-global
// aspect list (CajetaModule::getAspectClasses). A3's pointcut-
// matching pass walks this list at codegen time; this commit
// only ships the registry + the visitor hook.
//
// Tests inspect via the static accessor — no JIT bring-up needed.
//

#include "gtest/gtest.h"
#include "cajeta/compile/Compiler.h"
#include "cajeta/compile/CajetaModule.h"
#include "cajeta/type/CajetaClass.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

using cajeta::Compiler;
using cajeta::CajetaModule;
using cajeta::CajetaModulePtr;
using cajeta::CajetaClassPtr;

namespace {

CajetaModulePtr compileForInspection(Compiler& compiler,
                                     const std::string& source,
                                     const std::string& fqClassName) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = std::filesystem::temp_directory_path()
              / ("cajeta_a2_" + std::to_string(rng()));
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
                 / ("cajeta_a2_arch_" + std::to_string(rng()));
    std::filesystem::create_directories(archive);
    auto m = compiler.createModule(full.string(), base.string(), archive.string());
    compiler.compile(m);
    return m;
}

// Returns true if any aspect in the registry has the given short
// type name. The full canonical includes whatever package the user
// declared, which isn't worth pinning down in a presence check.
bool aspectRegistryContains(const std::string& shortName) {
    for (auto& a : CajetaModule::getAspectClasses()) {
        if (a && a->getQName()
                && a->getQName()->getTypeName() == shortName) {
            return true;
        }
    }
    return false;
}

} // namespace

// Bare @Aspect class registers. The Compiler ctor calls resetGlobals
// (which empties the aspect registry), then the parse pass walks
// visitClassDeclaration, observes the @Aspect annotation, and
// registers the class.
TEST(AspectRegistrationTests, bareAspectClassRegisters) {
    auto src =
        "package test;\n"
        "@Aspect public class AuditAspect {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.AuditAspect");
    EXPECT_TRUE(aspectRegistryContains("AuditAspect"));
}

// A class WITHOUT @Aspect doesn't end up in the registry. Catches
// the trivial false-positive case where any user class would show
// up if the visitor's check were too broad.
TEST(AspectRegistrationTests, nonAspectClassNotRegistered) {
    auto src =
        "package test;\n"
        "public class PlainOldClass {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.PlainOldClass");
    EXPECT_FALSE(aspectRegistryContains("PlainOldClass"));
}

// Multiple aspect classes in one source file each register, in
// declaration order. Insertion-order is the contract A7's @Order
// resolution will eventually layer on top of.
TEST(AspectRegistrationTests, multipleAspectsRegisterInOrder) {
    auto src =
        "package test;\n"
        "@Aspect public class FirstAspect {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n"
        "@Aspect public class SecondAspect {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n"
        "@Aspect public class ThirdAspect {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.FirstAspect");
    auto& list = CajetaModule::getAspectClasses();
    // At least our three, in the order written. Other aspects from
    // stdlib could in principle precede them; assert relative order
    // among ours.
    int idx1 = -1, idx2 = -1, idx3 = -1;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& name = list[i]->getQName()->getTypeName();
        if (name == "FirstAspect")  idx1 = (int) i;
        if (name == "SecondAspect") idx2 = (int) i;
        if (name == "ThirdAspect")  idx3 = (int) i;
    }
    ASSERT_NE(idx1, -1);
    ASSERT_NE(idx2, -1);
    ASSERT_NE(idx3, -1);
    EXPECT_LT(idx1, idx2);
    EXPECT_LT(idx2, idx3);
}

// @Aspect together with other annotations on the same class — the
// aspect registration fires regardless of what else is attached.
// Important because per the spec an aspect is typically also a
// @Component (for DI). The aspect registry shouldn't care about
// other annotations.
TEST(AspectRegistrationTests, aspectWithOtherAnnotationsStillRegisters) {
    auto src =
        "package test;\n"
        "@Component @Aspect public class LoggingAspect {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    Compiler compiler;
    compileForInspection(compiler, src, "test.LoggingAspect");
    EXPECT_TRUE(aspectRegistryContains("LoggingAspect"));
}

// Resetting the compiler clears the aspect registry. Each fresh
// Compiler instance starts with an empty list so cross-test state
// doesn't leak — same discipline the structureToModule map uses
// (cleared in resetGlobals).
TEST(AspectRegistrationTests, freshCompilerStartsEmpty) {
    // First compile populates the registry.
    {
        Compiler compiler;
        auto src =
            "package test;\n"
            "@Aspect public class FirstRun {\n"
            "    public static int32 run() { return 0; }\n"
            "}\n";
        compileForInspection(compiler, src, "test.FirstRun");
        EXPECT_TRUE(aspectRegistryContains("FirstRun"));
    }
    // Fresh Compiler — its ctor's resetGlobals call wipes the
    // registry. The previous test's aspect should no longer be
    // visible.
    {
        Compiler compiler;
        EXPECT_FALSE(aspectRegistryContains("FirstRun"));
    }
}
