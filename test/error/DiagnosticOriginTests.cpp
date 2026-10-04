// Diagnostics carry their origin (project, dependency, stdlib), each origin is capped on
// its own, and project diagnostics come first (diagnostic-origin spec 2 and 3).

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/error/DiagnosticEngine.h"
#include "cajeta/error/Diagnostics.h"

#include <string>

using cajeta::CollectedDiagnostic;
using cajeta::DiagnosticEngine;
using cajeta_test::CajetaJit;

namespace {

struct EngineScope {
    DiagnosticEngine engine;
    EngineScope() { DiagnosticEngine::setActive(&engine); }
    ~EngineScope() { DiagnosticEngine::setActive(nullptr); }
};

const char* kProjectSource =
    "package test;\n"
    "public class Keep {\n"
    "    public String v;\n"
    "    public Keep(String v) {\n"
    "        this.v = v;\n"
    "    }\n"
    "}\n"
    "public final class Ut {\n"
    "    public static int32 run() {\n"
    "        Keep k = heap Keep(\"\");\n"
    "        String s = \"abcdefghijklmnopqrstuvwxyz\";\n"
    "        String w #= s.substring(4, 20);\n"
    "        k.v = w;\n"
    "        return (int32) k.v.size();\n"
    "    }\n"
    "}\n";

}  // namespace

// 3.2.1: a stdlib flood past the cap never evicts a project diagnostic, which comes first.
TEST(DiagnosticOrigin, aStdlibFloodKeepsEveryProjectDiagnosticFirst) {
    DiagnosticEngine e;
    for (int i = 1; i <= 150; ++i) {
        e.report("warning", "W_STD", "noise", "<stdlib>/cajeta/x/A.cajeta", i, 1, "stdlib");
    }
    e.report("warning", "W_PROJ", "mine", "/p/U.cajeta", 5, 3, "project");
    auto out = e.finalize();
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front().code, "W_PROJ");
    EXPECT_EQ(out.front().origin, "project");
    int stdlibKept = 0;
    for (const auto& d : out) {
        if (d.origin == "stdlib" && d.code == "W_STD") ++stdlibKept;
    }
    EXPECT_EQ(stdlibKept, DiagnosticEngine::CAP);
}

// 3.2.2: a project flood still truncates to the cap with the trailing note.
TEST(DiagnosticOrigin, aProjectFloodStillTruncatesAtTheCap) {
    DiagnosticEngine e;
    for (int i = 1; i <= 120; ++i) {
        e.report("warning", "W_PROJ", "mine", "/p/U.cajeta", i, 1, "project");
    }
    auto out = e.finalize();
    int kept = 0;
    bool note = false;
    for (const auto& d : out) {
        if (d.code == "W_PROJ") ++kept;
        if (d.message.find("20 more") != std::string::npos) note = true;
    }
    EXPECT_EQ(kept, DiagnosticEngine::CAP);
    EXPECT_TRUE(note);
}

// 2.2.3: a JSON record carries its origin.
TEST(DiagnosticOrigin, aJsonRecordCarriesItsOrigin) {
    cajeta::setShowAllDiagnosticOrigins(true);
    DiagnosticEngine e;
    e.report("warning", "W_STD", "noise", "<stdlib>/cajeta/x/A.cajeta", 7, 1, "stdlib");
    testing::internal::CaptureStderr();
    e.emit(true);
    std::string err = testing::internal::GetCapturedStderr();
    cajeta::setShowAllDiagnosticOrigins(false);
    EXPECT_NE(err.find("\"origin\":\"stdlib\""), std::string::npos) << err;
}

// 2.2.1 and 2.2.2: in a real compile the stdlib's warnings are located and tagged stdlib,
// and the project's are tagged project.
TEST(DiagnosticOrigin, aCompileTagsStdlibAndProjectDiagnostics) {
    EngineScope scope;
    auto jit = CajetaJit::compile(kProjectSource, "test.Ut");
    ASSERT_NE(jit, nullptr);
    bool stdlibLocated = false;
    bool projectTagged = false;
    for (const auto& d : scope.engine.finalize()) {
        if (d.origin == "stdlib" && d.file.rfind("<stdlib>/cajeta/", 0) == 0 && d.line > 0) {
            stdlibLocated = true;
        }
        if (d.file.find("Ut.cajeta") != std::string::npos) {
            EXPECT_EQ(d.origin, "project") << d.code;
            projectTagged = true;
        }
    }
    EXPECT_TRUE(stdlibLocated) << "a stdlib warning should name its <stdlib>/ file and line";
    EXPECT_TRUE(projectTagged) << "the project's own warning should be collected";
}

namespace {

std::string emitText(const DiagnosticEngine& e) {
    testing::internal::CaptureStderr();
    e.emit(false);
    return testing::internal::GetCapturedStderr();
}

struct OriginsScope {
    explicit OriginsScope(bool all) { cajeta::setShowAllDiagnosticOrigins(all); }
    ~OriginsScope() { cajeta::setShowAllDiagnosticOrigins(false); }
};

}  // namespace

// 4.2.1: by default only project diagnostics are shown.
TEST(DiagnosticOrigin, theDefaultShowsOnlyProjectDiagnostics) {
    OriginsScope scope(false);
    DiagnosticEngine e;
    e.report("warning", "W_STD", "noise", "<stdlib>/cajeta/x/A.cajeta", 7, 1, "stdlib");
    e.report("warning", "W_DEP", "dep", "/dep/B.cajeta", 3, 1, "dependency");
    e.report("warning", "W_PROJ", "mine", "/p/U.cajeta", 5, 3, "project");
    std::string out = emitText(e);
    EXPECT_NE(out.find("W_PROJ"), std::string::npos) << out;
    EXPECT_EQ(out.find("W_STD"), std::string::npos) << out;
    EXPECT_EQ(out.find("W_DEP"), std::string::npos) << out;
}

// 4.2.2: asked for every origin, the stdlib's diagnostics are shown with their location.
TEST(DiagnosticOrigin, showingAllOriginsListsTheStdlibWithItsLocation) {
    OriginsScope scope(true);
    DiagnosticEngine e;
    e.report("warning", "W_STD", "noise", "<stdlib>/cajeta/x/A.cajeta", 7, 1, "stdlib");
    std::string out = emitText(e);
    EXPECT_NE(out.find("<stdlib>/cajeta/x/A.cajeta:7:1: W_STD"), std::string::npos) << out;
}

// 4.2.3: an error is shown whatever its origin.
TEST(DiagnosticOrigin, anErrorIsShownWhateverItsOrigin) {
    OriginsScope scope(false);
    DiagnosticEngine e;
    e.report("error", "E_DEP", "broken", "/dep/B.cajeta", 3, 1, "dependency");
    std::string out = emitText(e);
    EXPECT_NE(out.find("E_DEP"), std::string::npos) << out;
}

// 4.1: `--diag-origins=all` selects every origin, `=project` the default.
TEST(DiagnosticOrigin, theFlagSelectsTheOrigins) {
    OriginsScope scope(false);
    const char* all[] = {"cajeta", "--diag-origins=all"};
    cajeta::resolveDiagOriginsFromArgv(2, all);
    EXPECT_TRUE(cajeta::showAllDiagnosticOrigins());
    const char* project[] = {"cajeta", "--diag-origins=project"};
    cajeta::resolveDiagOriginsFromArgv(2, project);
    EXPECT_FALSE(cajeta::showAllDiagnosticOrigins());
}
