//
// script-units U5 (spec 6.1-6.3) — diagnostics, traces, tooling hygiene.
//
// The synthesized wrapper must be invisible: compile diagnostics carry the
// HOST's source name and the user's line (a line map built during synthesis
// translates wrapper lines back; unlocated semantic errors are stamped with
// the current statement's host line); runtime stack traces render script
// frames as `<script>` with host file + host line, never the synthesized
// class/entry names. The located Exception fields asserted here are exactly
// what --diag-format=json serializes (main.cpp emitException), so the JSON
// `file` requirement (5.3.1) rides the same data.
//

#include "gtest/gtest.h"
#include "JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

struct CompileError {
    std::string errorId;
    std::string file;
    int line = -1;
};

// `withLines` asks for --debug-info=full. Per-statement line marks became a
// full-debug-info feature when they were measured at 3.5-9.4x (see
// LineInfoCodegen::emitLineMark), so a DEFAULT build renders a script frame as
// `<script>` with the host FILE and no line. The host-line half of spec 6.2 is
// still exactly as it was, on request — the tests below assert both, so
// neither losing the line-map nor turning marks back on everywhere can pass.
std::unique_ptr<CajetaJit> compileScript(const std::string& source,
                                         const std::string& fqClass,
                                         const std::string& hostName,
                                         CompileError* err = nullptr,
                                         bool withLines = false) {
    CajetaJit::Options opts;
    opts.sessionHostName = hostName;
    opts.debugInfoEnabled = withLines;
    try {
        return CajetaJit::compile(source, fqClass, opts);
    } catch (cajeta::Exception& e) {
        if (err) {
            err->errorId = e.getErrorId();
            err->file = e.getFile();
            err->line = e.getLine();
        }
        return nullptr;
    }
}

}  // namespace

// 5.1.1 / spec 6.1 — an UNLOCATED semantic error (unassigned read throws
// with no location today) is stamped with the host source name and the
// erroring statement's HOST line, not a line inside the wrapper.

// 5.1.1 located form / spec 6.1 — an error thrown WITH a (wrapper) location
// has its line translated through the synthesis line map to the host line.
TEST(ScriptDiagnosticsTests, locatedErrorMapsToHostLine) {
    CompileError err;
    auto jit = compileScript(
        "public class P { public int32 v; public P(int32 v) { this.v = v; } }\n"
        "P p = heap P(1);\n"     // host line 2
        "P q = p;\n"             // host line 3 — borrow escape (located throw)
        "return 0;\n",
        "cajeta.script.diagtwo", "diag-cell-2", &err);
    EXPECT_EQ(nullptr, jit.get());
    EXPECT_EQ("CAJETA_ERROR_SESSION_BORROW_ESCAPE", err.errorId);
    EXPECT_EQ("diag-cell-2", err.file);
    EXPECT_EQ(3, err.line);
}

// 5.1.2 / spec 6.2 — a thrown-and-caught exception's rendered trace shows
// `<script>` frames carrying the host file and HOST line; the synthesized
// class canonical and `__cajeta_script_entry` never appear. Assertions run
// in-script over getStackTrace() (the StackFrameTrace pattern).
TEST(ScriptDiagnosticsTests, traceHidesSyntheticNames) {
    // Host layout (1-based): the throw sits inside `boom` on line 12.
    std::string src =
        "import cajeta.error.Exception;\n"                        // 1
        "import cajeta.error.StackFrame;\n"                       // 2
        "int32 code = 0;\n"                                       // 3
        "try {\n"                                                 // 4
        "    boom();\n"                                           // 5
        "} catch (Exception e) {\n"                               // 6
        "    StackFrame[] fs = e.getStackTrace();\n"              // 7
        "    if (fs.count() == 0) { code = 10; }\n"               // 8
        "    else { code = check(fs); }\n"                        // 9
        "}\n"                                                     // 10
        "return code;\n"                                          // 11
        "void boom() { throw heap Exception(\"x\"); }\n"          // 12
        "int32 check(StackFrame[] fs) {\n"                        // 13
        "    boolean sawScript = false;\n"
        "    for (int32 i = 0; i < fs.count(); i = i + 1) {\n"
        "        StackFrame f = fs[i];\n"
        "        if (f.declaringType.contains(\"diagtrace\")) { return 2; }\n"
        "        if (f.method.contains(\"__cajeta_script\")) { return 3; }\n"
        "        if (f.declaringType.contains(\"<script>\")) { sawScript = true; }\n"
        "    }\n"
        "    if (!sawScript) { return 4; }\n"
        "    StackFrame top = fs[0];\n"
        "    if (!top.method.contains(\"boom\")) { return 5; }\n"
        "    if (!top.file.contains(\"trace-cell\")) { return 6; }\n"
        "    if (top.line != $LINE) { return 7; }\n"
        "    return 1;\n"
        "}\n";
    // Substituted rather than prepended: the expected line is 12 because of
    // where the throw SITS, so anything that shifts the host layout by a line
    // invalidates the very thing being asserted.
    auto withExpected = [&src](const char* line) {
        std::string out = src;
        const std::string tok = "$LINE";
        auto at = out.find(tok);
        out.replace(at, tok.size(), line);
        return out;
    };

    // Default build: every synthetic-name and host-FILE assertion holds, and
    // the frame carries no line (0, which StackFrame documents as
    // "line-info unavailable").
    {
        auto jit = compileScript(withExpected("0"),
                                 "cajeta.script.diagtrace", "trace-cell");
        ASSERT_NE(nullptr, jit.get());
        auto entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
        ASSERT_NE(nullptr, entry);
        EXPECT_EQ(1, entry());
    }
    // --debug-info=full: the same frame, now carrying the HOST line. That is
    // the half spec 6.2 is actually about — a line pointing INTO the
    // synthesized wrapper would be worse than none, because it reads like a
    // real one.
    {
        auto jit = compileScript(withExpected("12"),
                                 "cajeta.script.diagtraceg", "trace-cell",
                                 nullptr, /*withLines=*/true);
        ASSERT_NE(nullptr, jit.get());
        auto entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
        ASSERT_NE(nullptr, entry);
        EXPECT_EQ(1, entry());
    }
}

// 5.1.3 / spec 6 lints — a binding written and never read fires NO
// diagnostic: the session holds it past the unit, so an unused-variable
// lint (none exists today; this pins the contract for when one does) must
// not treat it as dead.
