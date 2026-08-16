// external-debug Unit 2 — a frame descriptor names its DECLARING source file.
//
// #FrameDesc.fileName was emitted from `module->remappedSourcePath()`. The whole
// stdlib parses into ONE synthetic module whose source path is empty, so every
// stdlib frame rendered as `cajeta.lang.stream.Stream<int32>.forEach(:268)` —
// no file. That corrupts production exception traces, not just a debugger's
// view (spec external-debug §6).
//
// Pins:
//   2.1.1  A frame for a stdlib method names its file (Guid.cajeta), not "".
//   2.1.2  A user frame is unchanged.
//   2.1.3  Two classes declared in different stdlib files report different
//          files — the name is per-declaration, not per-module.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include "cajeta/type/CajetaClass.h"
#include "cajeta/type/CajetaType.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

    // The declaring file recorded on a canonical class, or "<missing>" if the
    // class never made it into the type map for this compile.
    std::string declaringFileOf(const std::string& name,
                                const std::string& package) {
        auto type = cajeta::CajetaType::of(name, package);
        if (!type) return "<missing>";
        auto cls = std::dynamic_pointer_cast<cajeta::CajetaClass>(type);
        if (!cls) return "<not-a-class>";
        return cls->getDeclaringFile();
    }

} // namespace

// 2.1.1 — a throw from inside Guid.parse: the top frame is cajeta.lang.Guid,
// and its file must be Guid.cajeta. This returned "" before Unit 2.
TEST(StackFrameFile, stdlibFrameNamesItsSourceFile) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
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
        "            if (!f.declaringType.contains(\"cajeta.lang.Guid\")) { return 2; }\n"
        "            if (!f.file.contains(\"Guid.cajeta\")) { return 4; }\n"
        "            if (f.line <= 0) { return 5; }\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n", "test.App");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}

// 2.1.2 — the user frame still names the user's file. No regression: user
// modules are one-file-per-module, so remappedSourcePath was always right there.
TEST(StackFrameFile, userFrameStillNamesItsSourceFile) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.error.Exception;\n"
        "import cajeta.error.StackFrame;\n"
        "public final class App {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            throw heap Exception(\"boom\");\n"
        "        } catch (Exception e) {\n"
        "            StackFrame[] fs = e.getStackTrace();\n"
        "            if (fs.count() == 0) { return 0; }\n"
        "            StackFrame f = fs[0];\n"
        "            if (!f.declaringType.contains(\"test.App\")) { return 2; }\n"
        "            if (!f.file.contains(\"App.cajeta\")) { return 3; }\n"
        "            if (f.line <= 0) { return 4; }\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n", "test.App");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 1);
}

// 2.1.3 — per-declaration, not per-module: every stdlib class shares one module,
// so if the file came from the module they would all report the same string
// (today: the same EMPTY string).
TEST(StackFrameFile, stdlibClassesInDifferentFilesReportDifferentFiles) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.lang.Guid;\n"
        "public final class App {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n", "test.App");
    ASSERT_NE(jit->lookup<int32_t (*)()>("run"), nullptr);

    auto guid   = declaringFileOf("Guid",   "cajeta.lang");
    auto string = declaringFileOf("String", "cajeta.lang");

    EXPECT_NE(guid, "");
    EXPECT_NE(string, "");
    EXPECT_NE(guid, string);
    EXPECT_NE(guid.find("Guid.cajeta"), std::string::npos) << guid;
    EXPECT_NE(string.find("String.cajeta"), std::string::npos) << string;
}

// 2.3.1 — the declaring file is IR-embedded (it lands in a #FrameDesc constant),
// so it must be the build-root-independent form: relative, never absolute.
TEST(StackFrameFile, declaringFileIsRootIndependent) {
    auto jit = CajetaJit::compile(
        "package test;\n"
        "import cajeta.lang.Guid;\n"
        "public final class App {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n", "test.App");
    ASSERT_NE(jit->lookup<int32_t (*)()>("run"), nullptr);

    auto guid = declaringFileOf("Guid", "cajeta.lang");
    ASSERT_NE(guid, "<missing>");
    EXPECT_NE(guid.rfind("/", 0), 0u)
        << "absolute path leaked into a FrameDesc: " << guid;
    EXPECT_EQ(guid.find(".."), std::string::npos) << guid;
}
