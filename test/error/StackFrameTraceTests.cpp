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
TEST(StackFrameTrace, topFrameResolvesUserLocation) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "import cajeta.error.FrameRole;\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            throw heap Exception(\"boom\");\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs = e.getStackTrace();\n"
        "            if (fs.count() == 0) { return 10; }\n"
        "            StackFrame f = fs[0];\n"
        "            String t = f.declaringType;\n"
        "            String m = f.method;\n"
        "            String fl = f.file;\n"
        "            int32 ln = f.line;\n"
        "            if (!t.contains(\"test.App\")) { return 2; }\n"
        "            if (!m.contains(\"run\")) { return 3; }\n"
        "            if (!fl.contains(\".cajeta\")) { return 4; }\n"
        "            if (ln <= 0) { return 5; }\n"
        "            if (f.role != FrameRole.User) { return 6; }\n"
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
TEST(StackFrameTrace, lineInfoOffYieldsAddressFrames) {
    CajetaJit::Options opts;
    opts.lineInfoEnabled = false;
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        try { throw heap Exception(\"boom\"); }\n"
        "        catch (Exception e) {\n"
        "            StackFrame[] fs = e.getStackTrace();\n"
        "            if (fs.count() == 0) { return 0; }\n"
        "            StackFrame f = fs[0];\n"
        "            int32 ln = f.line;\n"
        "            int64 a = f.nativeAddress;\n"
        "            if (ln == 0 && a != 0) { return 1; }\n"
        "            return 2;\n"
        "        }\n"
        "    }\n"
        "}\n", "test.App", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}
