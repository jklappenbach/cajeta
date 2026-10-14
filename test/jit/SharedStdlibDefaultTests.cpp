// Under stdlib reuse every compile links ONE materialized stdlib JITDylib
// (compiler-threadsafe 7c.3); a compile whose options bar reuse takes the
// fresh, fully isolated LLJIT.

#include "gtest/gtest.h"
#include "JitTestHelper.h"
#include "../PortableEnv.h"

#include <cstdint>
#include <cstdlib>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Sets CAJETA_STDLIB_REUSE for the test and restores it after.
struct ReuseEnv {
    bool had;
    std::string saved;
    explicit ReuseEnv(bool on) {
        const char* v = std::getenv("CAJETA_STDLIB_REUSE");
        had = v != nullptr;
        if (v) saved = v;
        set(on ? "1" : nullptr);
    }
    ~ReuseEnv() { set(had ? saved.c_str() : nullptr); }
    static void set(const char* v) {
        if (v) setenv("CAJETA_STDLIB_REUSE", v, 1);
        else unsetenv("CAJETA_STDLIB_REUSE");
    }
};

std::string classSource(const std::string& name, int value) {
    return "package test;\n"
           "public final class " + name + " {\n"
           "    public static int32 run() { return " + std::to_string(value) + "; }\n"
           "}\n";
}

int32_t run(CajetaJit& jit) { return jit.lookup<int32_t (*)()>("run")(); }

} // namespace

TEST(SharedStdlibDefaultTests, reusingCompilesLinkOneStdlib) {
    ReuseEnv reuse(true);
    auto a = CajetaJit::compile(classSource("A", 1), "test.A");
    EXPECT_TRUE(a->usesSharedStdlib());
    auto b = CajetaJit::compile(classSource("B", 2), "test.B");
    EXPECT_TRUE(b->usesSharedStdlib());
    EXPECT_EQ(CajetaJit::sharedStdlibMaterializations(), 1u);
    EXPECT_EQ(run(*a), 1);
    EXPECT_EQ(run(*b), 2);
}

TEST(SharedStdlibDefaultTests, reuseOffTakesTheFreshPath) {
    ReuseEnv reuse(false);
    auto c = CajetaJit::compile(classSource("C", 3), "test.C");
    EXPECT_FALSE(c->usesSharedStdlib());
    EXPECT_EQ(run(*c), 3);
}

TEST(SharedStdlibDefaultTests, anInstrumentedCompileTakesTheFreshPath) {
    ReuseEnv reuse(true);
    CajetaJit::Options opts;
    opts.profiler = cajeta::Profiler::Instrument;
    auto d = CajetaJit::compile(classSource("D", 4), "test.D", opts);
    EXPECT_FALSE(d->usesSharedStdlib());
    EXPECT_EQ(run(*d), 4);
}

TEST(SharedStdlibDefaultTests, stdlibStaticsStartFreshPerCompile) {
    ReuseEnv reuse(true);
    std::string prologue =
        "package test;\n"
        "import cajeta.xpu.Autotune;\n"
        "import cajeta.lang.Optional;\n";
    auto first = CajetaJit::compile(prologue +
        "public final class F {\n"
        "    public static int32 run() {\n"
        "        Autotune.remember(\"\", \"w\", 62);\n"
        "        Optional<int32> a = Autotune.recall(\"\", \"w\");\n"
        "        if (a.isPresent() && a.get() == 62) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n", "test.F");
    ASSERT_TRUE(first->usesSharedStdlib());
    EXPECT_EQ(run(*first), 1);
    first.reset();
    auto second = CajetaJit::compile(prologue +
        "public final class G {\n"
        "    public static int32 run() {\n"
        "        Optional<int32> a = Autotune.recall(\"\", \"w\");\n"
        "        if (a.isPresent()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n", "test.G");
    ASSERT_TRUE(second->usesSharedStdlib());
    EXPECT_EQ(run(*second), 0);
}

TEST(SharedStdlibDefaultTests, aTornDownCompileLeavesNoClassRegistration) {
    ReuseEnv reuse(true);
    auto first = CajetaJit::compile(
        "package test;\n"
        "public class Gone {\n"
        "    public Gone() { return; }\n"
        "}\n"
        "public final class H {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n", "test.H");
    ASSERT_TRUE(first->usesSharedStdlib());
    EXPECT_EQ(run(*first), 1);
    first.reset();
    auto second = CajetaJit::compile(
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.reflect.Class;\n"
        "public final class I {\n"
        "    public static int32 run() {\n"
        "        Optional<Class<?>> c = Class.forName(\"test.Gone\");\n"
        "        if (c.isPresent()) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n", "test.I");
    ASSERT_TRUE(second->usesSharedStdlib());
    EXPECT_EQ(run(*second), 0);
}

TEST(SharedStdlibDefaultTests, userCodeCatchesAStdlibThrow) {
    ReuseEnv reuse(true);
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base64;\n"
        "import cajeta.codec.Base64Exception;\n"
        "public final class E {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            int8[] raw #= Base64.decode(\"aa!a\");\n"
        "            return 0;\n"
        "        } catch (Base64Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    auto e = CajetaJit::compile(src, "test.E");
    ASSERT_TRUE(e->usesSharedStdlib());
    EXPECT_EQ(run(*e), 1);
}
