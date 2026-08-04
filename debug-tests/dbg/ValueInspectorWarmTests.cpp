//
// debug-type-sidecar Unit 5: the warm-path decode test — THE gap the suite
// lacked. Every pre-existing inspection test compiles fresh (TempProgram =
// always a cache miss), so the cache-HIT launch — the common IDE launch, where
// no type world exists — was never under test and regressed to `<unknown>`
// unnoticed. These tests drive a REAL whole-program cache slot: a cold -g
// launch populates it, a second launch hits it, and inspection at the warm
// stop must match the cold stop exactly.
//
// The bridge is table-only (Unit 2), and the hit reloads the global table from
// the slot's typeinfo sidecar (Unit 4) — so the warm decode below runs off
// sidecar bytes, not the type world (whatever the first launch left resident
// is unreachable by construction: ValueInspector holds no CajetaType path).
//
#include <gtest/gtest.h>

#include <chrono>
#include <map>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "cajeta/dbg/DebugTypeTable.h"
#include "cajeta/dbg/ValueInspector.h"
#include "cajeta/dbg/DebugVars.h"
#include "cajeta/dbg/DebugController.h"
#include "cajeta/jit/CajetaJitHost.h"
#include "../TempProgram.h"

namespace fs = std::filesystem;
using cajeta::debugtest::TempProgram;
using cajeta::jit::Breakpoint;
using cajeta::jit::JitRunOptions;
using cajeta::jit::JitRunResult;
using cajeta::jit::runJit;
using cajeta::jit::startDebugSession;
using cajeta::dbg::StopEvent;
using cajeta::dbg::ValueInspector;
using cajeta::dbg::ValueKind;

namespace {

// Array/object/ArrayList/HashMap/String locals — one of every decode shape.
const char* kProg =
    "package demo;\n"                                              // 1
    "public class Point { public int32 x; public int32 y; public static int32 made;\n" // 2
    "    public Point(int32 a, int32 b) { this.x = a; this.y = b; } }\n" // 3
    "public class Point3 extends Point { public int32 z;\n"         // 4
    "    public Point3(int32 a, int32 b, int32 c) { this.x = a; this.y = b; this.z = c; } }\n" // 5
    "public class Prog {\n"                                         // 6
    "    public static int32 main() {\n"                            // 7
    "        Point pt = heap Point(3, 4);\n"                        // 8
    "        int32[] nums = [3, 7, 9];\n"                           // 9
    "        String s = \"hi\";\n"                                  // 10
    "        ArrayList<int32> xs = [10, 20, 30];\n"                 // 11
    "        HashMap<int32,int32> m = [1:100, 2:200];\n"            // 12
    "        ArrayList<Point> ps = [heap Point(1, 2), heap Point(5, 6)];\n" // 13
    "        Point up = heap Point3(1, 2, 3); Point.made = 6;\n"    // 14
    "        int32 done = 0;\n"                                     // 15
    "        return 42;\n"                                          // 16 <-- bp
    "    }\n"                                                       // 13
    "}\n";                                                          // 14

// The decoded view of one stop: per local, the collapsed summary plus the
// first page of children (name, type, own summary).
struct DecodedStop {
    struct Child { std::string name, type, summary; };
    struct Local { std::string summary; std::vector<Child> children; };
    std::map<std::string, Local> locals;
};

// Park a session at Prog.cajeta:12, decode every local through `insp`, resume.
DecodedStop decodeAtStop(cajeta::jit::JitDebugSession& session,
                         ValueInspector& insp) {
    DecodedStop out;
    StopEvent ev;
    EXPECT_TRUE(session.controller().waitForStop(
        ev, std::chrono::seconds(60))) << "never hit the breakpoint";
    if (!ev.frameTop) return out;
    auto frames = cajeta::dbg::walkFrames(ev.frameTop);
    if (frames.empty()) return out;
    for (const auto& v : frames[0].locals) {
        DecodedStop::Local l;
        l.summary = insp.inspect(v.type, v.addr).summary;
        auto page = insp.children(v.type, v.addr);
        for (const auto& c : page.children)
            l.children.push_back({c.name, c.type,
                                  insp.inspect(c.type, c.addr).summary});
        out.locals[v.name] = std::move(l);
    }
    session.controller().resume();
    return out;
}

void expectStopsEqual(const DecodedStop& cold, const DecodedStop& warm) {
    ASSERT_EQ(cold.locals.size(), warm.locals.size());
    for (const auto& [name, cl] : cold.locals) {
        auto it = warm.locals.find(name);
        ASSERT_NE(it, warm.locals.end()) << "local missing warm: " << name;
        // HashMap summaries/rows depend on per-RUN hash order, and the cold
        // and warm stops are different executions — compare the entry count
        // and match rows BY NAME, not by position (a "{2 entries}" summary is
        // order-free already; element rows of lists/arrays are positional and
        // keep exact order because their names are the "[i]" indices).
        ASSERT_EQ(cl.children.size(), it->second.children.size()) << name;
        std::map<std::string, const DecodedStop::Child*> warmByName;
        for (const auto& c : it->second.children) warmByName[c.name] = &c;
        bool keyed = true;   // rows uniquely named (maps: keys; others: [i]/fields)
        if (warmByName.size() != it->second.children.size()) keyed = false;
        if (keyed) {
            EXPECT_EQ(cl.summary, it->second.summary) << name;
            for (const auto& c : cl.children) {
                auto w = warmByName.find(c.name);
                ASSERT_NE(w, warmByName.end()) << name << "." << c.name;
                EXPECT_EQ(c.type, w->second->type) << name << "." << c.name;
                EXPECT_EQ(c.summary, w->second->summary) << name << "." << c.name;
            }
        }
    }
}

struct WarmFixture {
    TempProgram prog{"demo", "Prog.cajeta", kProg};
    fs::path cacheDir;

    WarmFixture() {
        static std::mt19937_64 rng(std::random_device{}());
        cacheDir = fs::temp_directory_path()
                 / ("cajeta_warmdecode_test_" + std::to_string(rng()));
        fs::create_directories(cacheDir);
    }
    ~WarmFixture() {
        std::error_code ec;
        fs::remove_all(cacheDir, ec);
    }

    JitRunOptions opts() const {
        JitRunOptions o;
        o.sourceRoot = prog.sourceRoot();
        o.entryMethod = "demo.Prog.main";
        o.debugInfo = true;
        o.cacheDir = cacheDir.string();
        return o;
    }
};

} // namespace

// 5.1.1 (in-process form) — at a COLD stop, write the built table to a sidecar,
// reload it into a fresh table, and decode every local through the fresh table:
// identical to the global-table decode of the same stop.
TEST(ValueInspectorWarm, SidecarDrivenDecodeMatchesCold) {
    WarmFixture f;
    std::string err;
    auto session = startDebugSession(f.opts(), {Breakpoint{"Prog.cajeta", 16}},
                                     &err);
    ASSERT_NE(session, nullptr) << err;

    StopEvent ev;
    ASSERT_TRUE(session->controller().waitForStop(
        ev, std::chrono::seconds(60)));
    ASSERT_NE(ev.frameTop, nullptr);
    auto frames = cajeta::dbg::walkFrames(ev.frameTop);
    ASSERT_FALSE(frames.empty());

    // Round-trip the COLD-built global table through actual sidecar bytes.
    const fs::path side = f.cacheDir / "unit5.typeinfo";
    ASSERT_TRUE(cajeta::dbg::writeTypeSidecar(
        side.string(), cajeta::dbg::globalDebugTypeTable()));
    cajeta::dbg::DebugTypeTable fromDisk;
    ASSERT_TRUE(cajeta::dbg::loadTypeSidecar(side.string(), fromDisk));
    ASSERT_FALSE(fromDisk.empty());

    ValueInspector coldInsp(session->dataLayout());            // global table
    ValueInspector warmInsp(session->dataLayout(), fromDisk);  // disk table

    for (const auto& v : frames[0].locals) {
        auto c = coldInsp.inspect(v.type, v.addr);
        auto w = warmInsp.inspect(v.type, v.addr);
        EXPECT_EQ(c.summary, w.summary) << v.name;
        EXPECT_EQ(c.kind, w.kind) << v.name;
        EXPECT_NE(w.kind, ValueKind::Unknown) << v.name;
        auto cp = coldInsp.children(v.type, v.addr);
        auto wp = warmInsp.children(v.type, v.addr);
        ASSERT_EQ(cp.children.size(), wp.children.size()) << v.name;
        for (size_t i = 0; i < cp.children.size(); i++)
            EXPECT_EQ(coldInsp.inspect(cp.children[i].type, cp.children[i].addr).summary,
                      warmInsp.inspect(wp.children[i].type, wp.children[i].addr).summary)
                << v.name << "[" << i << "]";
    }

    session->controller().resume();
    EXPECT_EQ(session->join(), 42);
}

// 5.1.1/5.1.2 (authentic form) + acceptance 4.3.1 — a REAL cache-hit debug
// launch decodes every local identically to the cold launch of the same
// program: arrays, objects, collections and Strings all render, none are
// `<unknown>`. This is the IDE's warm launch, finally under test.
TEST(ValueInspectorWarm, CacheHitLaunchDecodesLikeCold) {
    WarmFixture f;
    std::vector<Breakpoint> bps{Breakpoint{"Prog.cajeta", 16}};

    // Cold session: populates the slot, and its stop is the reference decode.
    DecodedStop cold;
    {
        std::string err;
        auto session = startDebugSession(f.opts(), bps, &err);
        ASSERT_NE(session, nullptr) << err;
        ValueInspector insp(session->dataLayout(),
                            &session->resolvedTypeSymbols());
        cold = decodeAtStop(*session, insp);
        EXPECT_EQ(session->join(), 42);
    }
    ASSERT_FALSE(cold.locals.empty());
    // The reference itself must be a working decode, or equality proves nothing.
    ASSERT_EQ(cold.locals.at("pt").summary, "{x=3, y=4}");
    ASSERT_EQ(cold.locals.at("nums").summary, "[3, 7, 9]");
    ASSERT_EQ(cold.locals.at("s").summary, "\"hi\"");
    ASSERT_EQ(cold.locals.at("xs").children.size(), 3u);
    ASSERT_EQ(cold.locals.at("m").children.size(), 2u);
    ASSERT_EQ(cold.locals.at("ps").children.size(), 2u);
    ASSERT_EQ(cold.locals.at("ps").children[0].summary, "{x=1, y=2}");
    // runtime-type-inspection 5.1.1: the Point-declared local holds a Point3 —
    // COLD must already narrow (subclass summary, z + static `made` present).
    ASSERT_EQ(cold.locals.at("up").summary, "{x=1, y=2, z=3}");
    {
        bool sawZ = false, sawMade = false;
        for (const auto& c : cold.locals.at("up").children) {
            if (c.name == "z") sawZ = true;
            if (c.name == "made" && c.summary == "6") sawMade = true;
        }
        ASSERT_TRUE(sawZ) << "cold narrowing missing subclass field";
        ASSERT_TRUE(sawMade) << "cold statics missing";
    }

    // Prove the slot is HOT for these exact options before the warm session.
    JitRunResult probe;
    ASSERT_EQ(runJit(f.opts(), &probe), 42);
    ASSERT_TRUE(probe.cacheHit) << "slot not hot; the second session would "
                                   "recompile and prove nothing";

    // Warm session: a cache hit — no Compiler, no type world; the global
    // table comes from the slot's typeinfo sidecar (Unit 4).
    std::string err;
    auto session = startDebugSession(f.opts(), bps, &err);
    ASSERT_NE(session, nullptr) << err;
    // runtime-type-inspection 2.1.3 — warm resolution: the symbols came from
    // the sidecar-loaded table, the addresses from THIS run's LLJIT.
    {
        const auto& rs = session->resolvedTypeSymbols();
        ASSERT_FALSE(rs.vtableByAddr.empty())
            << "warm session resolved no vtable symbols";
        bool sawPoint = false;
        for (const auto& [addr, e] : rs.vtableByAddr)
            if (e.canonical == "demo.Point") sawPoint = true;
        EXPECT_TRUE(sawPoint);
    }
    ValueInspector insp(session->dataLayout(),
                        &session->resolvedTypeSymbols());
    DecodedStop warm = decodeAtStop(*session, insp);
    EXPECT_EQ(session->join(), 42);

    expectStopsEqual(cold, warm);
    // And explicitly: the regression this plan kills stays dead.
    EXPECT_NE(warm.locals.at("pt").summary, "<unknown>");
    EXPECT_FALSE(warm.locals.at("pt").children.empty());
    EXPECT_FALSE(warm.locals.at("xs").children.empty());
    EXPECT_FALSE(warm.locals.at("m").children.empty());
    // Class-typed collection elements (Julian's live report): the element ROW
    // itself must expand warm — its summary is a field peek, not <unknown>.
    ASSERT_EQ(warm.locals.at("ps").children.size(), 2u);
    EXPECT_EQ(warm.locals.at("ps").children[0].summary, "{x=1, y=2}");
    EXPECT_EQ(warm.locals.at("ps").children[1].summary, "{x=5, y=6}");
    // runtime-type-inspection 5.1.1 / 4.1.4 / 3.3.1: narrowing and statics
    // identical on the hit (expectStopsEqual compared them; pin the shape).
    EXPECT_EQ(warm.locals.at("up").summary, "{x=1, y=2, z=3}");
    {
        bool sawZ = false, sawMade = false;
        for (const auto& c : warm.locals.at("up").children) {
            if (c.name == "z") sawZ = true;
            if (c.name == "made" && c.summary == "6") sawMade = true;
        }
        EXPECT_TRUE(sawZ) << "warm narrowing missing subclass field";
        EXPECT_TRUE(sawMade) << "warm statics missing";
    }
}
