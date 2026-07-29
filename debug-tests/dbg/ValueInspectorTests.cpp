//
// debugger-variable-inspection Unit 1: the ValueInspector bridge — leaf vs
// aggregate classification and String text decode.
//
// Two layers, mirroring DebugVarsTests:
//  1) pure escape/quote — microsecond, no JIT.
//  2) a real stop: drive startDebugSession to a breakpoint, walk the frame for
//     String/array/primitive locals, and decode each through ValueInspector
//     using the session's live DataLayout. This is the authentic path — the
//     bridge reads the same layout the JIT'd code uses (spec §1.5).
//
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "cajeta/dbg/ValueInspector.h"
#include "cajeta/dbg/DebugVars.h"
#include "cajeta/dbg/DebugController.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/jit/CajetaJitHost.h"
#include "../TempProgram.h"

using cajeta::jit::Breakpoint;
using cajeta::jit::JitRunOptions;
using cajeta::jit::startDebugSession;
using cajeta::dbg::StopEvent;
using cajeta::dbg::ValueInspector;
using cajeta::dbg::ValueKind;
using cajeta::debugtest::TempProgram;

// ---- pure escape/quote (no JIT) ----

TEST(ValueInspectorEscape, QuotesPlainText) {
    EXPECT_EQ(cajeta::dbg::escapeAndQuote("hello"), "\"hello\"");
    EXPECT_EQ(cajeta::dbg::escapeAndQuote(""), "\"\"");
}

TEST(ValueInspectorEscape, EscapesControlAndQuote) {
    // newline, tab, quote, backslash render as escapes inside the quotes.
    EXPECT_EQ(cajeta::dbg::escapeAndQuote("a\nb"), "\"a\\nb\"");
    EXPECT_EQ(cajeta::dbg::escapeAndQuote("a\tb"), "\"a\\tb\"");
    EXPECT_EQ(cajeta::dbg::escapeAndQuote("a\"b"), "\"a\\\"b\"");
    EXPECT_EQ(cajeta::dbg::escapeAndQuote("a\\b"), "\"a\\\\b\"");
}

// ---- real stop: String decode + classification ----

namespace {
// Lines:              1              2                3
const char* kProg =
    "package demo;\n"                                        // 1
    "public class Box {\n"                                   // 2
    "    int32 v;\n"                                         // 3
    "    Box(int32 x) { this.v = x; }\n"                     // 4
    "}\n"                                                    // 5
    "public class Probe {\n"                                 // 6
    "    public static int32 main() {\n"                     // 7
    "        String tiny = \"hi\";\n"                        // 8  inline (<=12)
    "        String big = \"this is a long string\";\n"      // 9  windowed (>12)
    "        String esc = \"a\\nb\";\n"                      // 10 needs escaping
    "        int32 n = 5;\n"                                 // 11 primitive
    "        Box box = heap Box(7);\n"                       // 12 aggregate
    "        return n;\n"                                    // 13 <-- breakpoint
    "    }\n"                                                // 14
    "}\n";                                                   // 15

// Decode the named local's value through the inspector at a stop.
struct Probe {
    std::unique_ptr<cajeta::jit::JitDebugSession> session;
    const llvm::DataLayout* dl = nullptr;
    std::vector<cajeta::dbg::DbgFrameInfo> frames;

    const cajeta::dbg::DbgVar* local(const std::string& name) const {
        for (const auto& v : frames[0].locals)
            if (v.name == name) return &v;
        return nullptr;
    }
};
} // namespace

class ValueInspectorStop : public ::testing::Test {
protected:
    TempProgram prog{"demo", "Probe.cajeta", kProg};
    Probe p;

    void SetUp() override {
        JitRunOptions opts;
        opts.sourceRoot = prog.sourceRoot();
        opts.entryMethod = "demo.Probe.main";
        std::vector<Breakpoint> bps{ Breakpoint{"Probe.cajeta", 13} };
        std::string err;
        p.session = startDebugSession(opts, bps, &err);
        ASSERT_NE(p.session, nullptr) << err;
        StopEvent ev;
        ASSERT_TRUE(p.session->controller().waitForStop(
            ev, std::chrono::seconds(30))) << "never hit the breakpoint";
        ASSERT_NE(ev.frameTop, nullptr);
        p.dl = &p.session->dataLayout();
        p.frames = cajeta::dbg::walkFrames(ev.frameTop);
        ASSERT_FALSE(p.frames.empty());
    }

    void TearDown() override {
        if (p.session) {
            p.session->controller().resume();
            p.session->join();
        }
    }
};

TEST_F(ValueInspectorStop, DecodesInlineString) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("tiny");
    ASSERT_NE(v, nullptr);
    auto r = insp.inspect(v->type, v->addr);
    EXPECT_EQ(r.kind, ValueKind::Leaf);
    EXPECT_EQ(r.summary, "\"hi\"");
}

TEST_F(ValueInspectorStop, DecodesWindowedString) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("big");
    ASSERT_NE(v, nullptr);
    auto r = insp.inspect(v->type, v->addr);
    EXPECT_EQ(r.kind, ValueKind::Leaf);
    EXPECT_EQ(r.summary, "\"this is a long string\"");
}

TEST_F(ValueInspectorStop, EscapesStringText) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("esc");
    ASSERT_NE(v, nullptr);
    auto r = insp.inspect(v->type, v->addr);
    EXPECT_EQ(r.summary, "\"a\\nb\"");
}

TEST_F(ValueInspectorStop, ClassifiesLeafVsAggregate) {
    ValueInspector insp(*p.dl);
    // String and primitive are leaves.
    EXPECT_EQ(insp.inspect(p.local("tiny")->type, p.local("tiny")->addr).kind,
              ValueKind::Leaf);
    EXPECT_EQ(insp.inspect(p.local("n")->type, p.local("n")->addr).kind,
              ValueKind::Leaf);
    // An object is an aggregate (array decode is Unit 2).
    EXPECT_EQ(insp.inspect(p.local("box")->type, p.local("box")->addr).kind,
              ValueKind::Aggregate);
}

TEST_F(ValueInspectorStop, PrimitiveLeafRendersValue) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("n");
    auto r = insp.inspect(v->type, v->addr);
    EXPECT_EQ(r.summary, "5");
}

TEST_F(ValueInspectorStop, ResolvesFullyQualifiedAndShortNames) {
    ValueInspector insp(*p.dl);
    // Both the canonical and short type names resolve to a layout.
    EXPECT_TRUE(insp.canResolve("cajeta.lang.String"));
    EXPECT_TRUE(insp.canResolve("String"));
    // A garbage name resolves to nothing, no crash.
    EXPECT_FALSE(insp.canResolve("no.such.Type"));
}

TEST_F(ValueInspectorStop, NullStringIsSafe) {
    ValueInspector insp(*p.dl);
    // A slot holding a null String pointer never dereferences.
    void* nullSlot = nullptr;
    auto r = insp.inspect("cajeta.lang.String", &nullSlot);
    EXPECT_EQ(r.kind, ValueKind::Leaf);
    EXPECT_EQ(r.summary, "<null>");
}
