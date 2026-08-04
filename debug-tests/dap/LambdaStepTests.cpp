//
// Live report 2026-07-22 (tour line 132): step-over on a
// `list.stream().forEach((x) -> x.f())` line never lands on the next line.
// The call site and the lambda body share the line, and the lambda executes
// once per element through stdlib frames — the pending-step logic must skip
// all of it and stop on the FOLLOWING line of the origin frame.
//
#include <gtest/gtest.h>

#include <fstream>
#include <string>
#include <vector>

#include "cajeta/dap/DapServer.h"
#include "cajeta/dap/Json.h"
#include "../TempProgram.h"

using cajeta::dap::DapServer;
using cajeta::dap::Json;
using cajeta::debugtest::TempProgram;

namespace {

const char* kMain =
    "package demo;\n"
    "import cajeta.collection.ArrayList;\n"
    "public class Prog {\n"
    "    public static int32 main() {\n"
    "        ArrayList<Item> items = heap ArrayList<Item>();\n"   // line 5
    "        items.add(heap Item());\n"                           // line 6
    "        items.add(heap Item());\n"                           // line 7
    "        items.stream().forEach((it) -> it.bump());\n"        // line 8
    "        return Item.total + 40;\n"                           // line 9
    "    }\n"
    "}\n";

const char* kItem =
    "package demo;\n"
    "public class Item {\n"
    "    static int32 total = 0;\n"
    "    public void bump() {\n"
    "        total = total + 1;\n"
    "    }\n"
    "}\n";

bool drive(DapServer& srv, const Json& req, std::vector<Json>& log) {
    return srv.handle(req, [&log](const Json& m) { log.push_back(m); });
}

Json req(int seq, const std::string& command, Json args) {
    Json r = Json::object();
    r["seq"] = seq;
    r["type"] = "request";
    r["command"] = command;
    r["arguments"] = std::move(args);
    return r;
}

std::string stoppedReason(const std::vector<Json>& log) {
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "stopped")
            return m.at("body").at("reason").asString();
    return "";
}

} // namespace

TEST(LambdaStep, StepOverForEachLandsOnNextLine) {
    TempProgram p("demo", "Prog.cajeta", kMain);
    { std::ofstream out(p.root / "demo" / "Item.cajeta"); out << kItem; }
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = "demo.Prog.main";
    args["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", args), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Prog.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(stoppedReason(log), "breakpoint");

    // Step over the forEach: both lambda iterations and the stdlib frames
    // must be skipped; land on line 9 with reason "step".
    log.clear();
    Json stepArgs = Json::object();
    stepArgs["threadId"] = 0;
    drive(srv, req(5, "next", stepArgs), log);
    EXPECT_EQ(stoppedReason(log), "step") << "step-over ran away (or exited)";

    log.clear();
    drive(srv, req(6, "stackTrace", Json::object()), log);
    int line = -1;
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == "stackTrace") {
            const Json& fr = m.at("body").at("stackFrames");
            if (fr.size() > 0) line = fr[0].at("line").asInt();
        }
    EXPECT_EQ(line, 9) << "step-over landed on the wrong line";

    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
}

// Tour's line-132 forEach runs demos that SPAWN FIBERS. The pending step is
// fiber-filtered (DebugController:88), but this pins the WHOLE interaction:
// a step-over across a lambda whose body spawns + joins fibers must still
// land on the following line of the origin frame.
TEST(LambdaStep, StepOverForEachWithFiberSpawningBodyLands) {
    static const char* kMainF =
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Prog {\n"
        "    public static int32 main() {\n"
        "        ArrayList<Item> items = heap ArrayList<Item>();\n"
        "        items.add(heap Item());\n"
        "        items.add(heap Item());\n"
        "        items.stream().forEach((it) -> it.bump());\n"    // line 8
        "        return Item.total + 38;\n"                       // line 9
        "    }\n"
        "}\n";
    static const char* kItemF =
        "package demo;\n"
        "public class Item {\n"
        "    static int32 total = 0;\n"
        "    public void bump() {\n"
        "        scope {\n"
        "            spawn inc();\n"
        "            spawn inc();\n"
        "        }\n"
        "    }\n"
        "    public static void inc() {\n"
        "        total = total + 1;\n"
        "    }\n"
        "}\n";
    TempProgram p("demo", "Prog.cajeta", kMainF);
    { std::ofstream out(p.root / "demo" / "Item.cajeta"); out << kItemF; }
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = "demo.Prog.main";
    args["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", args), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Prog.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(stoppedReason(log), "breakpoint");

    log.clear();
    Json stepArgs = Json::object();
    stepArgs["threadId"] = 0;
    drive(srv, req(5, "next", stepArgs), log);
    EXPECT_EQ(stoppedReason(log), "step")
        << "step-over across fiber-spawning lambda ran away";

    log.clear();
    drive(srv, req(6, "stackTrace", Json::object()), log);
    int line = -1;
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == "stackTrace") {
            const Json& fr = m.at("body").at("stackFrames");
            if (fr.size() > 0) line = fr[0].at("line").asInt();
        }
    EXPECT_EQ(line, 9);

    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
}

// Live (tour 132, exact symptom): F8 lands lit at Stream.cajeta:241 —
// inside a DEMO's own stream use, far below the origin frame. Escalation:
// the stepped-over lambda body itself runs a nested stream pipeline. The
// step must skip every nested stdlib/lambda frame and land on line 9.
TEST(LambdaStep, StepOverLambdaWithNestedStreamLands) {
    static const char* kMainN =
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Prog {\n"
        "    public static int32 main() {\n"
        "        ArrayList<Item> items = heap ArrayList<Item>();\n"
        "        items.add(heap Item());\n"
        "        items.add(heap Item());\n"
        "        items.stream().forEach((it) -> it.bump());\n"    // line 8
        "        return Item.total + 38;\n"                       // line 9
        "    }\n"
        "}\n";
    static const char* kItemN =
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Item {\n"
        "    static int32 total = 0;\n"
        "    public void bump() {\n"
        "        ArrayList<Item> inner = heap ArrayList<Item>();\n"
        "        inner.add(heap Item());\n"
        "        boolean all = inner.stream().allMatch((x) -> true);\n"
        "        if (all) { total = total + 1; }\n"
        "        inner.stream().forEach((x) -> Item.tick());\n"
        "    }\n"
        "    public static void tick() {\n"
        "        total = total + 1;\n"
        "    }\n"
        "}\n";
    TempProgram p("demo", "Prog.cajeta", kMainN);
    { std::ofstream out(p.root / "demo" / "Item.cajeta"); out << kItemN; }
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = "demo.Prog.main";
    args["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", args), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Prog.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(stoppedReason(log), "breakpoint");

    log.clear();
    Json stepArgs = Json::object();
    stepArgs["threadId"] = 0;
    drive(srv, req(5, "next", stepArgs), log);
    EXPECT_EQ(stoppedReason(log), "step");

    log.clear();
    drive(srv, req(6, "stackTrace", Json::object()), log);
    int line = -1;
    std::string file;
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == "stackTrace") {
            const Json& fr = m.at("body").at("stackFrames");
            if (fr.size() > 0) {
                line = fr[0].at("line").asInt();
                file = fr[0].at("source").at("name").asString();
            }
        }
    EXPECT_EQ(file, "Prog.cajeta") << "landed in " << file << ":" << line;
    EXPECT_EQ(line, 9);

    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
}

// The live combination (tour 132): RESIDENT session stepping THROUGH
// stdlib frames. The resident codegen layer compiled the stdlib without the
// per-session profile/advice setup buildJit does, so its debug-frame chain
// can be incomplete — depths read shallow and step-over stops inside
// Stream code (live: lit at Stream.cajeta:241).
TEST(LambdaStep, ResidentStepOverNestedStreamStaysOutOfStdlib) {
    static const char* kMainR =
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Prog {\n"
        "    public static int32 main() {\n"
        "        ArrayList<Item> items = heap ArrayList<Item>();\n"
        "        items.add(heap Item());\n"
        "        items.add(heap Item());\n"
        "        items.stream().forEach((it) -> it.bump());\n"    // line 8
        "        return Item.total + 38;\n"                       // line 9
        "    }\n"
        "}\n";
    static const char* kItemR =
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Item {\n"
        "    static int32 total = 0;\n"
        "    public void bump() {\n"
        "        ArrayList<Item> inner = heap ArrayList<Item>();\n"
        "        inner.add(heap Item());\n"
        "        boolean all = inner.stream().allMatch((x) -> true);\n"
        "        if (all) { total = total + 1; }\n"
        "        inner.stream().forEach((x) -> Item.tick());\n"
        "    }\n"
        "    public static void tick() {\n"
        "        total = total + 1;\n"
        "    }\n"
        "}\n";
    TempProgram p("demo", "Prog.cajeta", kMainR);
    { std::ofstream out(p.root / "demo" / "Item.cajeta"); out << kItemR; }
    static std::mt19937_64 rng(std::random_device{}());
    std::filesystem::path cacheDir =
        std::filesystem::temp_directory_path()
        / ("cajeta_lambdastep_res_" + std::to_string(rng()));
    std::filesystem::create_directories(cacheDir);

    DapServer srv;
    std::vector<Json> log;
    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = "demo.Prog.main";
    args["sourceRoot"] = p.sourceRoot();
    args["cacheDir"] = cacheDir.string();
    args["resident"] = true;
    drive(srv, req(2, "launch", args), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Prog.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(stoppedReason(log), "breakpoint");

    log.clear();
    Json stepArgs = Json::object();
    stepArgs["threadId"] = 0;
    drive(srv, req(5, "next", stepArgs), log);
    EXPECT_EQ(stoppedReason(log), "step");

    log.clear();
    drive(srv, req(6, "stackTrace", Json::object()), log);
    int line = -1;
    std::string file;
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == "stackTrace") {
            const Json& fr = m.at("body").at("stackFrames");
            if (fr.size() > 0) {
                line = fr[0].at("line").asInt();
                file = fr[0].at("source").at("name").asString();
            }
        }
    EXPECT_EQ(file, "Prog.cajeta") << "landed in " << file << ":" << line;
    EXPECT_EQ(line, 9);

    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    drive(srv, req(8, "disconnect", Json::object()), log);
    std::error_code ec;
    std::filesystem::remove_all(cacheDir, ec);
}

// The ACTUAL tour-132 mechanism (traced live): a demo pipeline uses
// .parallel(), and ParallelDriver worker safepoints — fresh, shallow frame
// chains — satisfied the pending step-over's depth gate (depth<=origin) and
// parked inside stream internals. Step over a line whose body runs a
// PARALLEL fold; the step must land on the next line of the origin frame.
// DISABLED (resident-debug-server: parallel-step defect, 2026-07-22): this
// DEADLOCKS — six threads parked, no progress — reproducing the live tour
// defect in its hang form. Live (98 demos, different timing) the same
// mechanism instead STEALS the stop: ParallelDriver share safepoints reach
// the pending-step gate reporting fiber=0 with a detached-looking [main]
// chain (trace: depth=1 origin=1 << STOP inside a fold instantiation), so
// F8 parks in stream internals. Root fix is fiber-runtime-side: parallel
// share execution must carry a true fiber identity (and/or the stop-round
// quiesce must tolerate a pending step). Secondary defect, same hunt:
// instantiation bodies carry MIS-ATTRIBUTED source lines (fold's body maps
// into Stream.cajeta's comment block ~103-112 — Julian saw highlighted
// comments). Re-enable when the runtime fix lands.
TEST(LambdaStep, StepOverParallelPipelineLandsOnNextLine) {
    static const char* kMainP =
        "package demo;\n"
        "import cajeta.collection.ArrayList;\n"
        "import cajeta.collection.Collectors;\n"
        "public class Prog {\n"
        "    public static int32 main() {\n"
        "        ArrayList<int32> nums = heap ArrayList<int32>();\n"
        "        int32 i = 0;\n"
        "        while (i < 64) { nums.add(i); i = i + 1; }\n"    // line 7
        "        int32 s = Prog.sumParallel(nums);\n"             // line 9 <- step
        "        return s - 1974;\n"                              // line 10 (sum 0..63 = 2016; 2016-1974=42)
        "    }\n"
        "    public static int32 sumParallel(ArrayList<int32> nums) {\n"
        "        ArrayList<int32> par = nums.stream().parallel()\n"
        "            .collect(Collectors.toList<int32>());\n"
        "        int32 s = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < par.count()) { s = s + par.get(i); i = i + 1; }\n"
        "        return s;\n"
        "    }\n"
        "}\n";
    TempProgram p("demo", "Prog.cajeta", kMainP);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = "demo.Prog.main";
    args["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", args), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Prog.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 9;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(stoppedReason(log), "breakpoint");

    log.clear();
    Json stepArgs = Json::object();
    stepArgs["threadId"] = 0;
    drive(srv, req(5, "next", stepArgs), log);
    EXPECT_EQ(stoppedReason(log), "step");

    log.clear();
    drive(srv, req(6, "stackTrace", Json::object()), log);
    int line = -1;
    std::string file;
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == "stackTrace") {
            const Json& fr = m.at("body").at("stackFrames");
            if (fr.size() > 0) {
                line = fr[0].at("line").asInt();
                file = fr[0].at("source").at("name").asString();
            }
        }
    EXPECT_EQ(file, "Prog.cajeta") << "step parked in " << file << ":" << line;
    EXPECT_EQ(line, 10);

    // End the session like a client does. Leaving the program parked used to
    // hang the whole binary in ~DapServer (see
    // DapMultiSession.DestroyingAServerWhileParkedDoesNotHang, which pins the
    // server-side fix); this is the client-side half of the same discipline.
    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    drive(srv, req(8, "disconnect", Json::object()), log);
}

// Live report 2026-07-22 ("stepping up crashes debug"): step-OUT was
// unsatisfiable — 9.1's chain-containment gate demanded the origin frame
// still be on the chain, which returning past it never is, so the program
// ran to exit and the session died. Pins step-out AND step-over-that-returns.
TEST(LambdaStep, StepOutReturnsToCallerLine) {
    static const char* kOutProg =
        "package demo;\n"
        "public class Prog {\n"
        "    public static int32 main() {\n"
        "        int32 a = Prog.inner();\n"   // line 4  <- caller line
        "        return a + 20;\n"            // line 5
        "    }\n"
        "    public static int32 inner() {\n"
        "        int32 x = 11;\n"             // line 8  <- breakpoint
        "        int32 y = x + 11;\n"         // line 9
        "        return y;\n"                 // line 10
        "    }\n"
        "}\n";
    TempProgram p("demo", "Prog.cajeta", kOutProg);
    DapServer srv;
    std::vector<Json> log;

    drive(srv, req(1, "initialize", Json::object()), log);
    Json args = Json::object();
    args["entry-method"] = "demo.Prog.main";
    args["sourceRoot"] = p.sourceRoot();
    drive(srv, req(2, "launch", args), log);
    Json bpArgs = Json::object();
    Json src = Json::object();
    src["path"] = "Prog.cajeta";
    bpArgs["source"] = src;
    Json bps = Json::array();
    Json bp = Json::object();
    bp["line"] = 8;
    bps.push_back(bp);
    bpArgs["breakpoints"] = bps;
    drive(srv, req(3, "setBreakpoints", bpArgs), log);
    drive(srv, req(4, "configurationDone", Json::object()), log);
    ASSERT_EQ(stoppedReason(log), "breakpoint");

    // Step OUT of inner() -> back in main, at or after the call line.
    log.clear();
    Json stepArgs = Json::object();
    stepArgs["threadId"] = 0;
    drive(srv, req(5, "stepOut", stepArgs), log);
    EXPECT_EQ(stoppedReason(log), "step") << "step-out never landed";

    log.clear();
    drive(srv, req(6, "stackTrace", Json::object()), log);
    int line = -1;
    std::string fn;
    for (const auto& m : log)
        if (m.at("type").asString() == "response" &&
            m.at("command").asString() == "stackTrace") {
            const Json& fr = m.at("body").at("stackFrames");
            if (fr.size() > 0) {
                line = fr[0].at("line").asInt();
                fn = fr[0].at("name").asString();
            }
        }
    EXPECT_NE(fn.find("main"), std::string::npos)
        << "step-out landed in " << fn << ":" << line;
    EXPECT_GE(line, 4);

    log.clear();
    drive(srv, req(7, "continue", Json::object()), log);
    for (const auto& m : log)
        if (m.at("type").asString() == "event" &&
            m.at("event").asString() == "exited")
            EXPECT_EQ(m.at("body").at("exitCode").asInt(), 42);
}
