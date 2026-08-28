// xref-lint-emission-gap Unit 4 — CALL edges on the lint path.
//
// Unit 3 gave lint a resolved method body, which was enough for field
// references. Calls need one thing more: a callee. `MethodCallExpression` has
// no `resolveTypes` override at all, so under lint nothing ever resolves a
// callee and `CajetaClass::resolveMethod` — the choke point that records every
// edge — is never reached. Ctrl-click on a method resolves nowhere.
//
// Two consequences the tests below separate, because they have one cause but
// different symptoms:
//   * no `calls` records at all (4.1.1-4.1.7); and
//   * missing FIELD references, because a call's arguments do not live in
//     `children` (MethodCallExpression.h says so) and the default
//     `resolveTypes` walks only `children` — so `f(b.v)` never visits `b.v`
//     (4.1.8, raised by Unit 3's acceptance item 3.3.2).
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_XCE_DEVNULL "NUL"
#else
#  define CAJETA_XCE_DEVNULL "/dev/null"
#endif

std::string sourceRoot() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    return (envRoot && *envRoot) ? envRoot :
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        CAJETA_SOURCE_ROOT_DEFAULT;
#else
        ".";
#endif
}

std::string compilerBinary() { return sourceRoot() + "/build/src/cajeta"; }

bool haveCompiler() { return fs::exists(compilerBinary()); }

fs::path freshTempDir(const std::string& tag) {
    static std::mt19937_64 rng(std::random_device{}());
    auto base = fs::temp_directory_path()
              / ("cajeta_xce_" + tag + "_" + std::to_string(rng()));
    fs::create_directories(base);
    return base;
}

void writeUnit(const fs::path& root, const std::string& rel,
               const std::string& text) {
    auto file = root / rel;
    fs::create_directories(file.parent_path());
    std::ofstream(file) << text;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
}

int runQuiet(const std::string& cmd) {
    return std::system((cmd + " > " CAJETA_XCE_DEVNULL " 2>&1").c_str());
}

std::string lintRoot(const fs::path& root) {
    auto out = freshTempDir("doc") / "xref.json";
    runQuiet(compilerBinary() + " --lint " + root.string()
             + " --emit-xref=" + out.string() + " --diag-format=json");
    return slurp(out);
}

// Full build over the same root — the reference the lint path must match.
std::string buildExport(const fs::path& root, const std::string& entry) {
    auto tmp = freshTempDir("build");
    auto out = tmp / "xref.json";
    runQuiet(compilerBinary() + " --emit-xref=" + out.string()
             + " --emit=ir " + entry + " " + root.string() + " "
             + (tmp / "arch").string());
    return slurp(out);
}

// One record per line inside each relation's array (the writer's own
// determinism pin, XrefExport 1.1.7).
std::vector<std::string> relation(const std::string& doc, const std::string& name) {
    std::vector<std::string> out;
    const std::string key = "\"" + name + "\": [";
    auto at = doc.find(key);
    if (at == std::string::npos) return out;
    std::istringstream in(doc.substr(at + key.size()));
    for (std::string line; std::getline(in, line); ) {
        auto b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        auto e = line.find_last_not_of(" \t\r");
        std::string t = line.substr(b, e - b + 1);
        if (t.rfind("]", 0) == 0) break;
        if (!t.empty() && t.back() == ',') t.pop_back();
        if (t.rfind("{", 0) == 0) out.push_back(t);
    }
    return out;
}

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

int intField(const std::string& rec, const std::string& name) {
    auto at = rec.find("\"" + name + "\": ");
    if (at == std::string::npos) return -1;
    return std::atoi(rec.c_str() + at + name.size() + 4);
}

std::string strField(const std::string& rec, const std::string& name) {
    const std::string k = "\"" + name + "\": \"";
    auto at = rec.find(k);
    if (at == std::string::npos) return "";
    auto s = at + k.size();
    return rec.substr(s, rec.find('"', s) - s);
}

// A small corpus whose every method is reachable from the entry point, so the
// build export and the lint export cover the same code (the directional
// build-subset-of-lint rule Unit 1 established).
fs::path makeCorpus(const std::string& tag) {
    auto root = freshTempDir(tag);
    writeUnit(root, "demo/Counter.cajeta",
        "package demo;\n"                          // 1
        "public class Counter {\n"                 // 2
        "    int32 v;\n"                           // 3
        "    public Counter() { this.v = 0; }\n"   // 4
        "    public void bump() { this.v = this.v + 1; }\n"  // 5
        "    public int32 value() { return this.v; }\n"      // 6
        "}\n");                                              // 7
    return root;
}

} // namespace

// ── 4.1.1 — the reported defect, on the file it was reported against ──────
//
// `tour/lang/ClassesDemo.cajeta` calls `c.value()` twice, both times INSIDE a
// call argument. Positions are the build export's own, read from a full build
// of samples/tour on 2026-08-27 — the plan's original "17:47 and 18:26" was
// stale and is corrected there. Columns are 0-based.
TEST(XrefLintCallEdge, TheReportedClassesDemoCallsResolveUnderLint) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    const fs::path tour = fs::path(sourceRoot()) / "samples/tour/src/main/cajeta";
    if (!fs::exists(tour / "tour/lang/ClassesDemo.cajeta"))
        GTEST_SKIP() << "samples/tour not present";

    auto errFile = freshTempDir("classes") / "stderr.txt";
    (void) std::system((compilerBinary()
        + " --lint " + (tour / "tour/lang/ClassesDemo.cajeta").string()
        + " --source-root " + tour.string()
        + " --emit-xref --diag-format=json"
        + " > " CAJETA_XCE_DEVNULL " 2> " + errFile.string()).c_str());
    const std::string stream = slurp(errFile);
    ASSERT_FALSE(stream.empty()) << "per-edit lint produced no stream at all";

    bool at17 = false, at18 = false;
    std::istringstream in(stream);
    for (std::string line; std::getline(in, line); ) {
        if (!has(line, "\"rel\":\"calls\"")) continue;
        if (!has(line, "tour.Counter::value(pointer)")) continue;
        if (has(line, "\"line\": 17") && has(line, "\"col\": 61")) at17 = true;
        if (has(line, "\"line\": 18") && has(line, "\"col\": 19")) at18 = true;
    }
    EXPECT_TRUE(at17) << "no call edge to tour.Counter::value at 17:61 — this "
                         "is the reported defect: Ctrl-click on `value` in "
                         "`c.value()` resolves nowhere";
    EXPECT_TRUE(at18) << "no call edge to tour.Counter::value at 18:19";
}

// ── 4.1.2 — overloads keep distinct callee keys ───────────────────────────

TEST(XrefLintCallEdge, OverloadsAreNotConflated) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("overload");
    writeUnit(root, "demo/Ovl.cajeta",
        "package demo;\n"                                   // 1
        "public class Ovl {\n"                              // 2
        "    public static void f(int32 x) { }\n"           // 3
        "    public static void f(String s) { }\n"           // 4
        "    public static void run() {\n"                  // 5
        "        Ovl.f(1);\n"                               // 6
        "        Ovl.f(\"a\");\n"                           // 7
        "    }\n"                                           // 8
        "}\n");                                             // 9

    const auto calls = relation(lintRoot(root), "calls");
    ASSERT_FALSE(calls.empty()) << "no call edges emitted at all";

    std::set<std::string> keysAtSites;
    for (const auto& c : calls) {
        const int line = intField(c, "line");
        if (line == 6 || line == 7) keysAtSites.insert(strField(c, "callee"));
    }
    EXPECT_EQ(keysAtSites.size(), 2u)
        << "the two overloads of `f` must carry distinct callee keys; got "
        << keysAtSites.size() << " distinct key(s) across lines 6 and 7";
}

// ── 4.1.3 — a generic call names the TEMPLATE member ──────────────────────
//
// `b.put(1)` on a `Bag<int32>` resolves to a monomorphized instantiation that
// exists in no source file. The edge must name `demo.Bag::put`, which the
// developer can actually open (spec 2.2.3).
TEST(XrefLintCallEdge, AGenericCallNamesTheTemplateNotTheInstantiation) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("generic");
    writeUnit(root, "demo/Bag.cajeta",
        "package demo;\n"
        "public class Bag<T> {\n"
        "    public void put(T v) { }\n"
        "}\n");
    writeUnit(root, "demo/UseBag.cajeta",
        "package demo;\n"                                   // 1
        "public class UseBag {\n"                           // 2
        "    public static void run() {\n"                  // 3
        "        Bag<int32> b #= heap Bag<int32>();\n"      // 4
        "        b.put(1);\n"                               // 5
        "    }\n"                                           // 6
        "}\n");                                             // 7

    bool sawPut = false;
    for (const auto& c : relation(lintRoot(root), "calls")) {
        const std::string callee = strField(c, "callee");
        if (!has(callee, "put")) continue;
        sawPut = true;
        EXPECT_FALSE(has(callee, "<"))
            << "the edge names a monomorphized instantiation, which exists in "
               "no source file and opens nowhere: " << callee;
    }
    EXPECT_TRUE(sawPut) << "no call edge for `b.put(1)`";
}

// ── 4.1.4 — an unresolvable callee costs nothing ──────────────────────────

TEST(XrefLintCallEdge, AnUnresolvableCalleeEmitsNothingAndFailsNothing) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = makeCorpus("unresolvable");
    writeUnit(root, "demo/Bad.cajeta",
        "package demo;\n"
        "public class Bad {\n"
        "    public static void run() {\n"
        "        nosuchreceiver.nosuchmethod();\n"
        "    }\n"
        "}\n");

    const std::string doc = lintRoot(root);
    ASSERT_FALSE(doc.empty())
        << "one unresolvable callee sank the whole export";
    // The rest of the corpus still resolves...
    EXPECT_TRUE(has(doc, "\"demo.Counter\""))
        << "declarations were lost to an unresolvable callee";
    // ...and nothing dangling was guessed at for the bad call.
    for (const auto& c : relation(doc, "calls"))
        EXPECT_FALSE(has(strField(c, "callee"), "nosuchmethod"))
            << "a callee was guessed at rather than omitted: " << c;
}

// ── 4.1.5 — resolving twice still yields ONE edge ─────────────────────────
//
// The new `resolveTypes` override runs in the BUILD path too (codegen's
// pre-pass calls it), so every call site is now resolved twice: once resolving
// types, once generating code. `drainCalls` merges on position; this is the
// test that its key still collapses them instead of doubling every edge.
TEST(XrefLintCallEdge, ACallSiteResolvedTwiceYieldsOneEdge) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = makeCorpus("dedup");
    writeUnit(root, "demo/Main.cajeta",
        "package demo;\n"                                   // 1
        "public class Main {\n"                             // 2
        "    public static void run() {\n"                  // 3
        "        Counter c = stack Counter();\n"            // 4
        "        c.bump();\n"                               // 5
        "        c.bump();\n"                               // 6
        "    }\n"                                           // 7
        "}\n");                                             // 8

    for (const auto& [label, doc] : std::vector<std::pair<std::string, std::string>>{
             {"build", buildExport(root, "demo.Main.run")},
             {"lint",  lintRoot(root)}}) {
        ASSERT_FALSE(doc.empty()) << label << " export is empty";
        std::set<std::string> seen;
        for (const auto& c : relation(doc, "calls")) {
            const std::string key = strField(c, "file") + ":"
                                  + std::to_string(intField(c, "line")) + ":"
                                  + std::to_string(intField(c, "col")) + "->"
                                  + strField(c, "callee");
            EXPECT_TRUE(seen.insert(key).second)
                << label << " export carries a DUPLICATE call edge — the site "
                   "was recorded once per resolution instead of merged: " << key;
        }
        // Two distinct `c.bump()` sites on adjacent lines stay distinct.
        int bumps = 0;
        for (const auto& c : relation(doc, "calls"))
            if (has(strField(c, "callee"), "bump")) ++bumps;
        EXPECT_EQ(bumps, 2) << label << " export: two `c.bump()` call sites "
                               "must stay two edges, not merge into one";
    }
}

// ── 4.1.6 — a constructor call is an edge too ─────────────────────────────

TEST(XrefLintCallEdge, AConstructorCallIsRecorded) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = makeCorpus("ctor");
    writeUnit(root, "demo/MakeIt.cajeta",
        "package demo;\n"                                   // 1
        "public class MakeIt {\n"                           // 2
        "    public static void run() {\n"                  // 3
        "        Counter c = stack Counter();\n"            // 4
        "    }\n"                                           // 5
        "}\n");                                             // 6

    bool sawCtor = false;
    for (const auto& c : relation(lintRoot(root), "calls"))
        if (has(strField(c, "callee"), "demo.Counter::Counter")
            && has(strField(c, "file"), "MakeIt.cajeta")) sawCtor = true;
    EXPECT_TRUE(sawCtor)
        << "`stack Counter()` produced no call edge — Ctrl-click on a "
           "constructor resolves nowhere";
}

// ── 4.1.8 — a field access inside a call ARGUMENT ─────────────────────────
//
// Raised by Unit 3's acceptance item 3.3.2. Arguments are not in `children`
// (MethodCallExpression.h: "Method-call args aren't in `children`"), and the
// default `resolveTypes` walks only `children` — so today `take(b.v)` emits
// nothing while the identical access in `x = b.v` emits fine. This is what
// closes 3.3.2, and it is a FIELD assertion living here because the cause is
// Unit 4's.
TEST(XrefLintCallEdge, AFieldAccessInsideACallArgumentIsRecorded) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("callarg");
    writeUnit(root, "demo/Box.cajeta",
        "package demo;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box() { this.v = 1; }\n"
        "}\n");
    writeUnit(root, "demo/Args.cajeta",
        "package demo;\n"                                   // 1
        "public class Args {\n"                             // 2
        "    int32 sink;\n"                                 // 3
        "    public void bare(Box b) {\n"                   // 4
        "        this.sink = b.v;\n"                        // 5  works today
        "    }\n"                                           // 6
        "    public void inArg(Box b) {\n"                  // 7
        "        Args.take(b.v);\n"                         // 8  emits nothing
        "    }\n"                                           // 9
        "    public void inNestedArg(Box b) {\n"            // 10
        "        Args.take(b.v + 1);\n"                     // 11 emits nothing
        "    }\n"                                           // 12
        "    public static void take(int32 x) { }\n"        // 13
        "}\n");                                             // 14

    std::set<int> lines;
    for (const auto& r : relation(lintRoot(root), "references")) {
        if (!has(r, "\"kind\": \"field\"")) continue;
        if (!has(r, "demo.Box.v")) continue;
        if (!has(strField(r, "file"), "Args.cajeta")) continue;
        lines.insert(intField(r, "line"));
    }
    EXPECT_TRUE(lines.count(5))
        << "the plain-assignment access regressed";
    EXPECT_TRUE(lines.count(8))
        << "`take(b.v)` emitted no field reference — the walk never descends "
           "into call arguments (they are not in `children`)";
    EXPECT_TRUE(lines.count(11))
        << "`take(b.v + 1)` emitted no field reference";
}
