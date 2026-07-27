//
// debug-type-sidecar Unit 2: ValueInspector decodes through the table ALONE.
//
// The warm path is the point of this plan, and the thing that makes it hard to
// test is that a test process always has a live type world. So this test takes
// the table built from that world and RE-KEYS every record under names the type
// world cannot resolve ("warm$demo.Point"), then decodes the same live memory
// through the aliased table. If the bridge still reached for CajetaType, every
// lookup would fail and the decode would be `<unknown>` — passing here means the
// layout came from the table and nothing else (plan 2.1.2, spec §4.1.1).
//
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "cajeta/dbg/DebugTypeTable.h"
#include "cajeta/dbg/ValueInspector.h"
#include "cajeta/dbg/DebugVars.h"
#include "cajeta/dbg/DebugController.h"
#include "cajeta/jit/CajetaJitHost.h"
#include "cajeta/type/CajetaType.h"
#include "../TempProgram.h"

using cajeta::jit::Breakpoint;
using cajeta::jit::JitRunOptions;
using cajeta::jit::startDebugSession;
using cajeta::dbg::StopEvent;
using cajeta::dbg::ValueInspector;
using cajeta::dbg::ValueKind;
using cajeta::dbg::DebugTypeTable;
using cajeta::dbg::TypeRecord;
using cajeta::dbg::globalDebugTypeTable;
using cajeta::debugtest::TempProgram;

namespace {
const char* kProg =
    "package demo;\n"                                              // 1
    "public class Point { public int32 x; public int32 y;\n"        // 2
    "    public Point(int32 a, int32 b) { this.x = a; this.y = b; } }\n" // 3
    "public class Probe {\n"                                        // 4
    "    public static int32 main() {\n"                            // 5
    "        Point pt = heap Point(3, 4);\n"                        // 6
    "        int32[] nums = [3, 7, 9];\n"                           // 7
    "        String s = \"hi\";\n"                                  // 8
    "        ArrayList<int32> xs = [10, 20, 30];\n"                 // 9
    "        int32 done = 0;\n"                                     // 10
    "        return done;\n"                                        // 11 <-- bp
    "    }\n"                                                       // 12
    "}\n";                                                          // 13

// A name no type world can resolve. Arrays keep their `[]` suffix (the bridge
// classifies an array by the suffix, so the alias must still look like one).
std::string alias(const std::string& type) {
    if (type.empty()) return type;
    if (cajeta::dbg::isPrimitiveTypeName(type)) return type;  // decoded by name
    if (type.back() == ']') {
        auto lb = type.find_last_of('[');
        return alias(type.substr(0, lb)) + "[]";
    }
    return "warm$" + type;
}

// Re-key a table under alias names, remapping every field/element type it
// refers to, so a decode through it can only succeed via the table.
DebugTypeTable aliasTable(const DebugTypeTable& src) {
    DebugTypeTable out;
    out.setStringAbi(src.stringAbi());
    for (const auto& name : src.names()) {
        const TypeRecord* rec = src.find(name);
        if (!rec) continue;
        TypeRecord copy = *rec;
        copy.canonical = alias(name);
        for (auto& f : copy.fields) f.type = alias(f.type);
        copy.elem.type = alias(copy.elem.type);
        out.put(std::move(copy));
    }
    return out;
}
} // namespace

class ValueInspectorTableTest : public ::testing::Test {
protected:
    TempProgram prog{"demo", "Probe.cajeta", kProg};
    std::unique_ptr<cajeta::jit::JitDebugSession> session;
    const llvm::DataLayout* dl = nullptr;
    std::vector<cajeta::dbg::DbgFrameInfo> frames;
    DebugTypeTable warm;

    const cajeta::dbg::DbgVar* local(const std::string& name) const {
        for (const auto& v : frames[0].locals)
            if (v.name == name) return &v;
        return nullptr;
    }

    void SetUp() override {
        JitRunOptions opts;
        opts.sourceRoot = prog.sourceRoot();
        opts.entryMethod = "demo.Probe.main";
        std::vector<Breakpoint> bps{ Breakpoint{"Probe.cajeta", 11} };
        std::string err;
        session = startDebugSession(opts, bps, &err);
        ASSERT_NE(session, nullptr) << err;
        StopEvent ev;
        ASSERT_TRUE(session->controller().waitForStop(
            ev, std::chrono::seconds(30))) << "never hit the breakpoint";
        ASSERT_NE(ev.frameTop, nullptr);
        dl = &session->dataLayout();
        frames = cajeta::dbg::walkFrames(ev.frameTop);
        ASSERT_FALSE(frames.empty());

        // The session populated the global table cold; alias it into a table
        // whose names are dead to the type world.
        ASSERT_FALSE(globalDebugTypeTable().empty())
            << "the cold build did not populate the global type table";
        warm = aliasTable(globalDebugTypeTable());
    }

    void TearDown() override {
        if (session) {
            session->controller().resume();
            session->join();
        }
    }
};

TEST_F(ValueInspectorTableTest, DecodesThroughTableOnly) {
    ValueInspector insp(*dl, warm);

    // Precondition: these names are unknown to the live type world, so any
    // decode below that succeeded via CajetaType would be impossible.
    EXPECT_EQ(cajeta::CajetaType::of("warm$demo.Point"), nullptr);
    EXPECT_EQ(cajeta::CajetaType::of("warm$cajeta.lang.String"), nullptr);

    // A class: fields at their table offsets, values read from live memory.
    const auto* pt = local("pt");
    ASSERT_NE(pt, nullptr);
    const std::string ptType = alias(pt->type);
    auto page = insp.children(ptType, pt->addr);
    ASSERT_EQ(page.children.size(), 2u);
    EXPECT_EQ(page.children[0].name, "x");
    EXPECT_EQ(page.children[1].name, "y");
    EXPECT_EQ(insp.inspect(page.children[0].type, page.children[0].addr).summary, "3");
    EXPECT_EQ(insp.inspect(page.children[1].type, page.children[1].addr).summary, "4");
    EXPECT_EQ(insp.inspect(ptType, pt->addr).summary, "{x=3, y=4}");

    // An array: element type, stride and storage from the record.
    const auto* nums = local("nums");
    ASSERT_NE(nums, nullptr);
    auto arr = insp.children(alias(nums->type), nums->addr);
    ASSERT_EQ(arr.children.size(), 3u);
    EXPECT_EQ(insp.inspect(arr.children[0].type, arr.children[0].addr).summary, "3");
    EXPECT_EQ(insp.inspect(arr.children[2].type, arr.children[2].addr).summary, "9");
    EXPECT_EQ(insp.inspect(alias(nums->type), nums->addr).summary, "[3, 7, 9]");

    // A String: leaf, decoded by the ABI the table carries.
    const auto* s = local("s");
    ASSERT_NE(s, nullptr);
    auto sv = insp.inspect(alias(s->type), s->addr);
    EXPECT_EQ(sv.kind, ValueKind::Leaf);
    EXPECT_EQ(sv.summary, "\"hi\"");

    // A collection: the logical view, selected by the record's collection kind
    // (not by matching the type name) and walked through aliased field types.
    const auto* xs = local("xs");
    ASSERT_NE(xs, nullptr);
    auto list = insp.children(alias(xs->type), xs->addr);
    ASSERT_EQ(list.children.size(), 3u);
    EXPECT_EQ(list.children[0].name, "[0]");
    EXPECT_EQ(insp.inspect(list.children[0].type, list.children[0].addr).summary, "10");
    EXPECT_EQ(insp.inspect(list.children[2].type, list.children[2].addr).summary, "30");
}

// A type the table does not carry degrades cleanly — no fault, no guessed
// layout (spec §2.2.3, §5.1.1).
TEST_F(ValueInspectorTableTest, UncarriedTypeIsUnknownNotAFault) {
    DebugTypeTable emptyTable;
    ValueInspector insp(*dl, emptyTable);
    const auto* pt = local("pt");
    ASSERT_NE(pt, nullptr);

    auto r = insp.inspect(pt->type, pt->addr);
    EXPECT_EQ(r.kind, ValueKind::Unknown);
    EXPECT_EQ(r.summary, "<unknown>");
    EXPECT_TRUE(insp.children(pt->type, pt->addr).children.empty());
    EXPECT_FALSE(insp.canResolve(pt->type));

    // An array with no record: aggregate, but nothing walked.
    const auto* nums = local("nums");
    ASSERT_NE(nums, nullptr);
    auto a = insp.inspect(nums->type, nums->addr);
    EXPECT_EQ(a.kind, ValueKind::Aggregate);
    EXPECT_EQ(a.summary, "<null>");
    EXPECT_TRUE(insp.children(nums->type, nums->addr).children.empty());

    // Primitives never needed the table.
    EXPECT_TRUE(insp.canResolve("int32"));
    const auto* done = local("done");
    ASSERT_NE(done, nullptr);
    EXPECT_EQ(insp.inspect(done->type, done->addr).summary, "0");
}
