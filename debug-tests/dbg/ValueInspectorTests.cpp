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
using cajeta::dbg::Storage;
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

// ---- Unit 2: array decode over the three storage forms ----

namespace {
// Lines:               1               2                 3
const char* kArrProg =
    "package demo;\n"                                            // 1
    "public class Point {\n"                                     // 2
    "    int32 x;\n"                                             // 3
    "    int32 y;\n"                                             // 4
    "    Point(int32 a, int32 b) { this.x = a; this.y = b; }\n" // 5
    "}\n"                                                        // 6
    "public class Probe {\n"                                     // 7
    "    public static int32 main() {\n"                         // 8
    "        int32[] nums = [3, 7, 9];\n"                        // 9  primitive inline
    "        String[] strs = [\"a\", \"bb\", \"ccc\"];\n"        // 10 64-byte slots
    "        Point[] pts = [heap Point(1, 2), heap Point(3, 4)];\n" // 11 ptr slots
    "        int32[] big = [0,1,2,3,4,5,6,7,8,9];\n"             // 12 paging
    "        int32 done = 0;\n"                                  // 13
    "        return done;\n"                                     // 14 <-- breakpoint
    "    }\n"                                                    // 15
    "}\n";                                                       // 16
} // namespace

class ValueInspectorArrayStop : public ::testing::Test {
protected:
    TempProgram prog{"demo", "Probe.cajeta", kArrProg};
    Probe p;

    void SetUp() override {
        JitRunOptions opts;
        opts.sourceRoot = prog.sourceRoot();
        opts.entryMethod = "demo.Probe.main";
        std::vector<Breakpoint> bps{ Breakpoint{"Probe.cajeta", 14} };
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

TEST_F(ValueInspectorArrayStop, DecodesPrimitiveArray) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("nums");
    ASSERT_NE(v, nullptr);
    auto page = insp.children(v->type, v->addr);
    ASSERT_EQ(page.children.size(), 3u);
    EXPECT_EQ(page.remaining, 0u);
    EXPECT_EQ(page.children[0].name, "[0]");
    EXPECT_EQ(page.children[1].name, "[1]");
    EXPECT_EQ(page.children[0].storage, Storage::Inline);
    // values read from data + i*stride
    EXPECT_EQ(insp.inspect(page.children[0].type, page.children[0].addr).summary, "3");
    EXPECT_EQ(insp.inspect(page.children[1].type, page.children[1].addr).summary, "7");
    EXPECT_EQ(insp.inspect(page.children[2].type, page.children[2].addr).summary, "9");
    // int32 stride is 4 bytes, inline
    auto d0 = reinterpret_cast<char*>(page.children[0].addr);
    auto d1 = reinterpret_cast<char*>(page.children[1].addr);
    EXPECT_EQ(d1 - d0, 4);
}

TEST_F(ValueInspectorArrayStop, DecodesStringArray) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("strs");
    ASSERT_NE(v, nullptr);
    auto page = insp.children(v->type, v->addr);
    ASSERT_EQ(page.children.size(), 3u);
    EXPECT_EQ(page.children[0].type, "cajeta.lang.String");
    EXPECT_EQ(page.children[0].storage, Storage::Pointer);
    // each element decodes to its text (the String* lives at the slot base)
    EXPECT_EQ(insp.inspect(page.children[0].type, page.children[0].addr).summary, "\"a\"");
    EXPECT_EQ(insp.inspect(page.children[1].type, page.children[1].addr).summary, "\"bb\"");
    EXPECT_EQ(insp.inspect(page.children[2].type, page.children[2].addr).summary, "\"ccc\"");
    // slot stride is the full String struct (64-byte inline slot), not a bare ptr
    auto d0 = reinterpret_cast<char*>(page.children[0].addr);
    auto d1 = reinterpret_cast<char*>(page.children[1].addr);
    EXPECT_GT(d1 - d0, 8);
}

TEST_F(ValueInspectorArrayStop, DecodesRefClassArray) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("pts");
    ASSERT_NE(v, nullptr);
    auto page = insp.children(v->type, v->addr);
    ASSERT_EQ(page.children.size(), 2u);
    EXPECT_EQ(page.children[0].type, "demo.Point");
    EXPECT_EQ(page.children[0].storage, Storage::Pointer);
    // A reference class carries STRUCT_FLAG, so — like String's 64-byte slots —
    // its array elements are struct-sized inline slots with the instance
    // pointer at the slot base (stride from DataLayout, not a bare 8-byte ptr).
    auto d0 = reinterpret_cast<char*>(page.children[0].addr);
    auto d1 = reinterpret_cast<char*>(page.children[1].addr);
    EXPECT_GT(d1 - d0, 8);
    // Each slot base holds a real Point* — two DISTINCT heap pointers (an inline
    // struct would instead show the SAME vtable word in both slots).
    void* p0 = *reinterpret_cast<void**>(page.children[0].addr);
    void* p1 = *reinterpret_cast<void**>(page.children[1].addr);
    EXPECT_NE(p0, nullptr);
    EXPECT_NE(p1, nullptr);
    EXPECT_NE(p0, p1);
    EXPECT_GT(reinterpret_cast<uintptr_t>(p0), 0x10000u);
    // fields are Unit 3; here it just classifies as an aggregate
    EXPECT_EQ(insp.inspect(page.children[0].type, page.children[0].addr).kind,
              ValueKind::Aggregate);
}

TEST_F(ValueInspectorArrayStop, ReadsLengthFromHeader) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("nums");
    ASSERT_NE(v, nullptr);
    auto page = insp.children(v->type, v->addr, 0, 100);
    EXPECT_EQ(page.children.size() + page.remaining, 3u);
    // ≤5 elements inline in the summary
    EXPECT_EQ(insp.inspect(v->type, v->addr).summary, "[3, 7, 9]");
}

TEST_F(ValueInspectorArrayStop, PagesLargeArray) {
    ValueInspector insp(*p.dl);
    const auto* v = p.local("big");   // 10 elements
    ASSERT_NE(v, nullptr);
    auto p0 = insp.children(v->type, v->addr, 0, 4);
    ASSERT_EQ(p0.children.size(), 4u);
    EXPECT_EQ(p0.remaining, 6u);
    EXPECT_EQ(p0.nextStart, 4u);
    EXPECT_EQ(p0.children[0].name, "[0]");
    auto p1 = insp.children(v->type, v->addr, p0.nextStart, 4);
    ASSERT_EQ(p1.children.size(), 4u);
    EXPECT_EQ(p1.children[0].name, "[4]");
    EXPECT_EQ(p1.remaining, 2u);
    auto p2 = insp.children(v->type, v->addr, p1.nextStart, 4);
    ASSERT_EQ(p2.children.size(), 2u);
    EXPECT_EQ(p2.remaining, 0u);
    EXPECT_EQ(p2.children[0].name, "[8]");
    // >5 elements → count summary
    EXPECT_EQ(insp.inspect(v->type, v->addr).summary, "{10 elements}");
}

TEST_F(ValueInspectorArrayStop, NullArrayIsSafe) {
    ValueInspector insp(*p.dl);
    // A slot holding a null array pointer yields zero children and never derefs.
    void* nullSlot = nullptr;
    auto page = insp.children("int32[]", &nullSlot);
    EXPECT_TRUE(page.children.empty());
    EXPECT_EQ(page.remaining, 0u);
    auto r = insp.inspect("int32[]", &nullSlot);
    EXPECT_EQ(r.kind, ValueKind::Aggregate);
    EXPECT_EQ(r.summary, "<null>");
}
