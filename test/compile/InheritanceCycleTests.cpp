// A class that is its own ancestor is CAJETA_ERROR_CYCLIC_INHERITANCE (plan
// field-store-ownership 1.2.5). Before, the struct layout recursed until the stack ran out.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;
namespace fs = std::filesystem;

namespace {

const char* kRun =
    "public final class D {\n"
    "    public static int32 run() { return 0; }\n"
    "}\n";

std::string expectCycle(const std::string& body) {
    try {
        CajetaJit::compile("package test;\n" + body + kRun, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_CYCLIC_INHERITANCE") << e.getMessage();
        return e.getMessage();
    } catch (const std::exception& e) {
        ADD_FAILURE() << "expected CAJETA_ERROR_CYCLIC_INHERITANCE, got " << e.what();
        return e.what();
    }
    ADD_FAILURE() << "expected CAJETA_ERROR_CYCLIC_INHERITANCE, compiled cleanly";
    return "";
}

std::string compilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r = (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        CAJETA_SOURCE_ROOT_DEFAULT;
#else
        ".";
#endif
    return r + "/build/src/cajeta";
}

}  // namespace

TEST(InheritanceCycleTests, aClassThatExtendsItselfIsRejected) {
    std::string msg = expectCycle(
        "public class C extends C {\n"
        "    public C() { }\n"
        "}\n");
    EXPECT_NE(msg.find("test.C"), std::string::npos) << msg;
}

TEST(InheritanceCycleTests, aTwoClassCycleIsRejectedAndNamed) {
    std::string msg = expectCycle(
        "public class A extends B {\n"
        "    public A() { }\n"
        "}\n"
        "public class B extends A {\n"
        "    public B() { }\n"
        "}\n");
    EXPECT_NE(msg.find("test.A"), std::string::npos) << msg;
    EXPECT_NE(msg.find("test.B"), std::string::npos) << msg;
}

TEST(InheritanceCycleTests, aThreeClassCycleIsRejected) {
    expectCycle(
        "public class P extends Q {\n"
        "    public P() { }\n"
        "}\n"
        "public class Q extends R {\n"
        "    public Q() { }\n"
        "}\n"
        "public class R extends P {\n"
        "    public R() { }\n"
        "}\n");
}

TEST(InheritanceCycleTests, aDeepChainWithoutACycleCompiles) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "public class L0 {\n"
        "    public int64 v;\n"
        "    public L0() { this.v = 1L; }\n"
        "}\n"
        "public class L1 extends L0 {\n"
        "    public L1() { }\n"
        "}\n"
        "public class L2 extends L1 {\n"
        "    public L2() { }\n"
        "}\n"
        "public class L3 extends L2 {\n"
        "    public L3() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { L3 x = heap L3(); return (int32) x.v; }\n"
        "}\n",
        "test.D");
    ASSERT_NE(jit, nullptr);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
}

TEST(InheritanceCycleTests, lintReportsTheCycleInsteadOfCrashing) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "no compiler binary";
    static std::mt19937_64 rng(std::random_device{}());
    auto root = fs::temp_directory_path() / ("cajeta_cyc_" + std::to_string(rng()));
    fs::create_directories(root / "src" / "test");
    std::ofstream(root / "src" / "test" / "A.cajeta")
        << "package test;\n"
           "public class A extends B {\n    public A() { }\n}\n"
           "public class B extends A {\n    public B() { }\n}\n";
    auto out = root / "out.txt";
    std::string cmd = compilerBinary() + " --lint " + (root / "src").string() + " --emit-xref="
                    + (root / "x.json").string() + " > " + out.string() + " 2>&1";
    int rc = std::system(cmd.c_str());
    std::ifstream f(out);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("SIGSEGV"), std::string::npos) << text;
    EXPECT_NE(text.find("is its own ancestor: test."), std::string::npos) << text;
    EXPECT_NE(rc, 0);
}

// A stray `Object` whose path gives it another package must not take itself as `Object`.
TEST(InheritanceCycleTests, aMispackagedObjectDoesNotBecomeItsOwnParent) {
    if (!fs::exists(compilerBinary())) GTEST_SKIP() << "no compiler binary";
    static std::mt19937_64 rng(std::random_device{}());
    auto root = fs::temp_directory_path() / ("cajeta_obj_" + std::to_string(rng()));
    fs::create_directories(root / "src" / "copy" / "lang");
    std::ofstream(root / "src" / "copy" / "lang" / "Object.cajeta")
        << "package cajeta.lang;\npublic class Object {\n    public Object() { }\n}\n";
    auto out = root / "out.txt";
    std::string cmd = compilerBinary() + " --lint " + (root / "src").string() + " --emit-xref="
                    + (root / "x.json").string() + " > " + out.string() + " 2>&1";
    (void) std::system(cmd.c_str());
    std::ifstream f(out);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find("SIGSEGV"), std::string::npos) << text;
    EXPECT_EQ(text.find("is its own ancestor"), std::string::npos) << text;
}
