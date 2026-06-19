//
// IfxFacadeTests — cajeta.ifx window-domain contract (STACK item: "Surface + Window +
// WindowEvent contract"). The facade is platform-agnostic, so these run on any host (incl. this
// Windows box). They pin: (1) cajeta.ifx parses on demand via the IfxInfo anchor, and (2) the
// window-domain value types construct and expose their fields.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/compile/Compiler.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;
using cajeta::Compiler;

// cajeta.ifx is parsed on demand (lazy stdlib loader): importing IfxInfo triggers exactly that
// package's parse, and the anchor method runs end-to-end.
TEST(IfxFacadeTests, ifxPackageParsesOnDemandViaIfxInfo) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.IfxInfo;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return IfxInfo.version();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);   // IfxInfo.version() == 1
    EXPECT_TRUE(Compiler::stdlibPackageParsed("cajeta.ifx"));
}

// Surface is an opaque value type — it constructs and reports its drawable extent.
TEST(IfxFacadeTests, surfaceConstructsAndReportsExtent) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.Surface;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Surface s = heap Surface(0, 5, 1280, 720);\n"   // nativeHandle 0 => headless
        "        return (int32) s.surfaceWidth();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1280);
}

// WindowEvent is the portable event value type — it constructs and reports its kind.
TEST(IfxFacadeTests, windowEventConstructsAndReportsType) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.WindowEvent;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WindowEvent e = heap WindowEvent(6, 0, 0.0f, 0.0f, 0, 0);\n"   // 6 == CLOSE
        "        return e.eventType();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 6);
}

// Window is the contract handle type — it constructs and returns its opaque handle.
TEST(IfxFacadeTests, windowConstructsAndReturnsHandle) {
    std::string src =
        "package test;\n"
        "import cajeta.ifx.Window;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Window w = heap Window(42);\n"
        "        return (int32) w.handle();\n"
        "    }\n"
        "}\n";

    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}
