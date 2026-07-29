// slices plan 4.1.8 / 4.2.3 — the escape-resolution visibility lint: each
// silent resolve at a String field store reports an informational NOTE
// (CAJETA_LINT_SLICE_RESOLVED) to the active DiagnosticEngine during codegen.
//
// Asserted in-process (an engine installed around a JIT compile): the CLI
// delivery surface is the diagnostics roadmap's own remaining sub-phase
// ("run codegen in collect mode; extend collect-and-continue to full
// compile" — docs/CompilerModes.md) — when that lands, this note rides it
// with no further changes here. Never an error: compilation succeeds.

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/error/DiagnosticEngine.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

struct EngineScope {
    cajeta::DiagnosticEngine engine;
    EngineScope() { cajeta::DiagnosticEngine::setActive(&engine); }
    ~EngineScope() { cajeta::DiagnosticEngine::setActive(nullptr); }
};

bool hasCode(const cajeta::DiagnosticEngine& eng, const std::string& code,
             std::string* sev = nullptr) {
    for (auto& d : eng.finalize()) {
        if (d.code == code) {
            if (sev) *sev = d.severity;
            return true;
        }
    }
    return false;
}

}  // namespace

// 4.1.8 — a local slice stored into an outliving field resolves silently;
// the NOTE is reported (severity "note" — never an error) and compilation
// succeeds.
TEST(SliceLint, ResolvedStoreEmitsNote) {
    std::string src =
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
        "        String w = s.substring(4, 20);\n"
        "        k.v = w;\n"
        "        return (int32) k.v.size();\n"
        "    }\n"
        "}\n";
    EngineScope scope;
    auto jit = CajetaJit::compile(src, "test.Ut");
    ASSERT_NE(jit, nullptr);
    std::string sev;
    EXPECT_TRUE(hasCode(scope.engine, "CAJETA_LINT_SLICE_RESOLVED", &sev))
        << "expected the resolve-site note";
    EXPECT_EQ(sev, "note");
    EXPECT_FALSE(scope.engine.hasErrors()) << "the lint must never be an error";
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 16);
}

// The negative: a local-only slice (borrow — nothing resolves) reports no note.
TEST(SliceLint, LocalOnlySliceIsQuiet) {
    std::string src =
        "package test;\n"
        "public final class Ut {\n"
        "    public static int32 run() {\n"
        "        String s = \"abcdefghijklmnopqrstuvwxyz\";\n"
        "        String w = s.substring(4, 20);\n"
        "        return (int32) w.size();\n"
        "    }\n"
        "}\n";
    EngineScope scope;
    auto jit = CajetaJit::compile(src, "test.Ut");
    ASSERT_NE(jit, nullptr);
    EXPECT_FALSE(hasCode(scope.engine, "CAJETA_LINT_SLICE_RESOLVED"))
        << "a borrow-only slice must not report a resolution";
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 16);
}
