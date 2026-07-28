//
// debugger-runtime-type-inspection Unit 3: runtime-type decode.
//
// A reference row's instance carries its vtable pointer at slot 0; matching it
// against the session-resolved vtable map names the RUNTIME type, and the row
// decodes that type's full inherited-then-own field set — the tour's
// `Shape sq = stack Square(5)` finally shows `side`. A base-view pointer into
// a multi-parent object is rebased by the entry's sub-object offset before
// field offsets apply. Anything unmatched falls back to declared-type decode,
// silently (spec §2.1.3–2.1.6, §2.2.2–2.2.3).
//
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

#include "cajeta/dbg/DebugTypeTable.h"
#include "cajeta/dbg/ValueInspector.h"
#include "cajeta/dbg/DebugVars.h"
#include "cajeta/dbg/DebugController.h"
#include "cajeta/jit/CajetaJitHost.h"
#include "../TempProgram.h"

using cajeta::jit::Breakpoint;
using cajeta::jit::JitRunOptions;
using cajeta::jit::startDebugSession;
using cajeta::dbg::StopEvent;
using cajeta::dbg::ValueInspector;
using cajeta::dbg::ValueKind;
using cajeta::debugtest::TempProgram;

namespace {
const char* kProg =
    "package demo;\n"                                              // 1
    "public class Shape { public int32 id;\n"                       // 2
    "    public Shape() { this.id = 0; }\n"                         // 3
    "    public int32 area() { return 0; } }\n"                     // 4
    "public class Square extends Shape { public int32 side;\n"      // 5
    "    public Square(int32 s) { this.id = 1; this.side = s; }\n"  // 6
    "    public int32 area() { return this.side * this.side; } }\n" // 7
    "public class Circle extends Shape { public int32 radius;\n"    // 8
    "    public Circle(int32 r) { this.id = 2; this.radius = r; }\n" // 9
    "    public int32 area() { return 3 * this.radius * this.radius; } }\n" // 10
    "public class A { public int32 a; public A() { return; } }\n"   // 11
    "public class B { public int32 b; public B() { return; } }\n"   // 12
    "public class C extends A, B {\n"                               // 13
    "    public C() { this.a = 7; this.b = 11; } }\n"               // 14
    "public record Vec2 { int32 a; int32 b; }\n"                    // 15
    "public class Probe {\n"                                        // 16
    "    public static int32 main() {\n"                            // 17
    "        Shape s1 = heap Square(5);\n"                          // 18
    "        Shape s2 = heap Circle(3);\n"                          // 19
    "        C c = heap C();\n"                                     // 20
    "        B bref = c;\n"                                         // 21
    "        ArrayList<Shape> shapes = [heap Square(2), heap Circle(9)];\n" // 22
    "        Vec2 v = {a:8, b:9};\n"                                // 23
    "        String str = \"hi\";\n"                                // 24
    "        int32 done = 0;\n"                                     // 25
    "        return done;\n"                                        // 26 <-- bp
    "    }\n"                                                       // 27
    "}\n";                                                          // 28

const cajeta::dbg::InspectedChild* childNamed(
        const cajeta::dbg::ChildPage& page, const std::string& name) {
    for (const auto& c : page.children) if (c.name == name) return &c;
    return nullptr;
}
} // namespace

class ValueInspectorRuntimeTest : public ::testing::Test {
protected:
    TempProgram prog{"demo", "Probe.cajeta", kProg};
    std::unique_ptr<cajeta::jit::JitDebugSession> session;
    const llvm::DataLayout* dl = nullptr;
    std::vector<cajeta::dbg::DbgFrameInfo> frames;

    const cajeta::dbg::DbgVar* local(const std::string& name) const {
        for (const auto& v : frames[0].locals)
            if (v.name == name) return &v;
        return nullptr;
    }

    ValueInspector inspector() {
        return ValueInspector(*dl, cajeta::dbg::globalDebugTypeTable(),
                              &session->resolvedTypeSymbols());
    }

    void SetUp() override {
        JitRunOptions opts;
        opts.sourceRoot = prog.sourceRoot();
        opts.entryMethod = "demo.Probe.main";
        std::vector<Breakpoint> bps{ Breakpoint{"Probe.cajeta", 26} };
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
    }
    void TearDown() override {
        if (session) { session->controller().resume(); session->join(); }
    }
};

// 3.1.1 — a Shape local holding a Square reports demo.Square, summarizes from
// Square's fields, and expands to the FULL inherited-then-own set.
TEST_F(ValueInspectorRuntimeTest, SubclassBehindBaseRow) {
    auto insp = inspector();
    const auto* v = local("s1");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(v->type, "demo.Shape");   // declared type on the wire

    EXPECT_EQ(insp.runtimeType(v->type, v->addr), "demo.Square");
    auto r = insp.inspect(v->type, v->addr);
    EXPECT_EQ(r.kind, ValueKind::Aggregate);
    EXPECT_EQ(r.summary, "{id=1, side=5}");   // inherited then own

    auto page = insp.children(v->type, v->addr);
    ASSERT_EQ(page.children.size(), 2u);
    EXPECT_EQ(page.children[0].name, "id");
    EXPECT_EQ(page.children[1].name, "side");
    EXPECT_EQ(insp.inspect(page.children[1].type, page.children[1].addr).summary, "5");

    const auto* v2 = local("s2");
    ASSERT_NE(v2, nullptr);
    EXPECT_EQ(insp.runtimeType(v2->type, v2->addr), "demo.Circle");
    EXPECT_EQ(insp.inspect(v2->type, v2->addr).summary, "{id=2, radius=3}");
}

// 3.1.2 — a B-typed reference to a C extends A, B: the interior pointer is
// rebased by the secondary vtable's sub-object offset, and ALL of C's fields
// decode at their true offsets. Without the rebase this reads garbage.
TEST_F(ValueInspectorRuntimeTest, MultiParentInteriorPointerRebases) {
    auto insp = inspector();
    const auto* v = local("bref");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(insp.runtimeType(v->type, v->addr), "demo.C");
    auto page = insp.children(v->type, v->addr);
    const auto* fa = childNamed(page, "a");
    const auto* fb = childNamed(page, "b");
    ASSERT_NE(fa, nullptr) << "A's field missing through the B view";
    ASSERT_NE(fb, nullptr);
    EXPECT_EQ(insp.inspect(fa->type, fa->addr).summary, "7");
    EXPECT_EQ(insp.inspect(fb->type, fb->addr).summary, "11");
    // And the whole-object view matches the direct C-typed local.
    const auto* cv = local("c");
    ASSERT_NE(cv, nullptr);
    EXPECT_EQ(insp.inspect(v->type, v->addr).summary,
              insp.inspect(cv->type, cv->addr).summary);
}

// 3.1.3 — a slot-0 word matching no vtable falls back SILENTLY to
// declared-type decode: no fault, no guessed layout, declared type reported.
TEST_F(ValueInspectorRuntimeTest, UnmatchedVtableFallsBack) {
    auto insp = inspector();
    // A fake "instance" whose slot-0 word is garbage.
    alignas(8) uint64_t fakeInst[4] = {0xdeadbeefcafeull, 42, 0, 0};
    void* fakeSlot = &fakeInst[0];
    EXPECT_EQ(insp.runtimeType("demo.Shape", &fakeSlot), "demo.Shape");
    auto r = insp.inspect("demo.Shape", &fakeSlot);
    EXPECT_EQ(r.kind, ValueKind::Aggregate);   // declared record still drives
    auto page = insp.children("demo.Shape", &fakeSlot);
    EXPECT_EQ(page.children.size(), 1u);       // Shape's declared field only
    // Null slot / null instance: unchanged.
    void* nullSlot = nullptr;
    EXPECT_EQ(insp.runtimeType("demo.Shape", &nullSlot), "demo.Shape");
    EXPECT_TRUE(insp.children("demo.Shape", &nullSlot).children.empty());
}

// 3.1.4 — element rows narrow too: an ArrayList<Shape> reports each element's
// runtime type, and expanding one shows the subclass's fields.
TEST_F(ValueInspectorRuntimeTest, CollectionElementRowsNarrow) {
    auto insp = inspector();
    const auto* v = local("shapes");
    ASSERT_NE(v, nullptr);
    auto page = insp.children(v->type, v->addr);
    ASSERT_EQ(page.children.size(), 2u);
    EXPECT_EQ(page.children[0].type, "demo.Square");
    EXPECT_EQ(page.children[1].type, "demo.Circle");
    auto inner = insp.children(page.children[0].type, page.children[0].addr);
    const auto* side = childNamed(inner, "side");
    ASSERT_NE(side, nullptr);
    EXPECT_EQ(insp.inspect(side->type, side->addr).summary, "2");
}

// 3.1.5 — value types and String are untouched by narrowing.
TEST_F(ValueInspectorRuntimeTest, ValueTypesAndStringUnchanged) {
    auto insp = inspector();
    const auto* v = local("v");
    ASSERT_NE(v, nullptr);
    EXPECT_EQ(insp.runtimeType(v->type, v->addr), v->type);  // no vtable word
    EXPECT_EQ(insp.inspect(v->type, v->addr).summary, "{a=8, b=9}");
    const auto* s = local("str");
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(insp.inspect(s->type, s->addr).summary, "\"hi\"");
    EXPECT_EQ(insp.inspect(s->type, s->addr).kind, ValueKind::Leaf);
}
