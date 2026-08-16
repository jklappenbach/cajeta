// diagnostic-exceptions Unit 3: semantic stack traces via the line-info shadow
// stack. getStackTrace() resolves each frame to declaringType/method/file/line/
// role with no debug info (mechanism B). Line-info is on by default.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>
using cajeta_test::CajetaJit;

// 3.1.1 — JIT/debug default (line-info on): the top frame of a throw in
// test.App.run resolves to test.App / run / *.cajeta / line>0 / role=User.

// 3.1.2 — a throw originating inside a cajeta.* stdlib method (Guid.parse)
// yields a top frame with declaringType in cajeta.lang and role=Stdlib.
TEST(StackFrameTrace, stdlibFrameCarriesStdlibRole) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "import cajeta.error.FrameRole;\n"
        "import cajeta.lang.Guid;\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            Guid g #= Guid.parse(\"not-a-valid-guid\");\n"
        "            return 9;\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs = e.getStackTrace();\n"
        "            if (fs.count() == 0) { return 0; }\n"
        "            StackFrame f = fs[0];\n"
        "            String dt = f.declaringType;\n"
        "            if (!dt.contains(\"cajeta.lang.Guid\")) { return 2; }\n"
        "            if (f.role != FrameRole.Stdlib) { return 3; }\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n", "test.App");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}

// 3.1.4 — --line-info=off: frames fall back to raw addresses (line==0,
// nativeAddress!=0), no crash.
