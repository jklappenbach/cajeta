// xref-lint-emission-gap Unit 5 — the contract the earlier units made true.
//
// Three properties, each raised by a MEASUREMENT rather than by review:
//
//   5.1.1  captureStaticReceivers is NOT subsumed by body resolution. Measured
//          2026-08-28 by disabling it over samples/tour: 363 type references
//          disappear and none reappear. Body resolution records a static
//          receiver's CALL edge and FIELD reference but never a type reference
//          for the receiver identifier, so Ctrl-click on the word `Math` in
//          `Math.abs(x)` would resolve nowhere. It stays; this pins that.
//   5.1.2  A field access inside a LAMBDA body. The single field reference the
//          build found and lint did not, across the whole tour.
//   5.1.3  A generic METHOD on a NON-generic owner must name the same callee on
//          both paths.
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifdef _WIN32
#  define CAJETA_XLC_DEVNULL "NUL"
#else
#  define CAJETA_XLC_DEVNULL "/dev/null"
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
              / ("cajeta_xlc_" + tag + "_" + std::to_string(rng()));
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
    return std::system((cmd + " > " CAJETA_XLC_DEVNULL " 2>&1").c_str());
}

std::string lintRoot(const fs::path& root) {
    auto out = freshTempDir("doc") / "xref.json";
    runQuiet(compilerBinary() + " --lint " + root.string()
             + " --emit-xref=" + out.string() + " --diag-format=json");
    return slurp(out);
}

std::string buildExport(const fs::path& root, const std::string& entry) {
    auto tmp = freshTempDir("build");
    auto out = tmp / "xref.json";
    runQuiet(compilerBinary() + " --emit-xref=" + out.string()
             + " --emit=ir " + entry + " " + root.string() + " "
             + (tmp / "arch").string());
    return slurp(out);
}

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

} // namespace

// ── 5.1.1 — static receivers still produce TYPE references ────────────────
//
// The guard on retiring captureStaticReceivers. `Odometer.built` and
// `Math.abs(x)` must each leave a type reference on the receiver identifier,
// which is what Ctrl-click on the TYPE NAME resolves through. Body resolution
// does not produce these — it produces the field reference and the call edge.
TEST(XrefLintContract, AStaticReceiverLeavesATypeReferenceOnTheTypeName) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("staticrecv");
    writeUnit(root, "demo/Reg.cajeta",
        "package demo;\n"                                   // 1
        "public class Reg {\n"                              // 2
        "    public static int32 count;\n"                  // 3
        "    public static int32 bump() { return 1; }\n"    // 4
        "}\n");                                             // 5
    writeUnit(root, "demo/UseReg.cajeta",
        "package demo;\n"                                   // 1
        "public class UseReg {\n"                           // 2
        "    int32 sink;\n"                                 // 3
        "    public void run() {\n"                         // 4
        "        this.sink = Reg.count;\n"                  // 5  field receiver
        "        this.sink = Reg.bump();\n"                 // 6  call receiver
        "    }\n"                                           // 7
        "}\n");                                             // 8

    const std::string doc = lintRoot(root);
    ASSERT_FALSE(doc.empty());

    int typeRefsToReg = 0;
    for (const auto& r : relation(doc, "references")) {
        if (!has(r, "\"kind\": \"type\"")) continue;
        if (strField(r, "target") != "demo.Reg") continue;
        if (!has(strField(r, "file"), "UseReg.cajeta")) continue;
        const int line = intField(r, "line");
        if (line == 5 || line == 6) ++typeRefsToReg;
    }
    EXPECT_EQ(typeRefsToReg, 2)
        << "a static receiver must leave a TYPE reference on the type name — "
           "body resolution records the field reference and the call edge but "
           "NOT this, so Ctrl-click on the word `Reg` would resolve nowhere. "
           "If captureStaticReceivers was retired, this is what it cost.";
}

// ── 5.1.2 — a field access inside a LAMBDA body ───────────────────────────
//
// A bare-identifier lambda (`(q) -> q.x`) parses with EMPTY paramTypes, so the
// parameter scope `LambdaExpression::resolveTypes` pushes was never pushed for
// one and its whole body went unresolved. The types come from the enclosing
// call's formal, so this works only because Unit 5 resolves the CALLEE before
// its arguments and pins the formal via setExpectedType.
//
// Both shapes here have a receiver whose type is known outright. The remaining
// shape — a CHAINED generic receiver, `pts.stream().fold(...)`, where the
// receiver's type is an unsubstituted template return (`Stream<T>`, not
// `Stream<Pt>`) — is NOT covered, and is filed as 5.1.4 with its measurement.
TEST(XrefLintContract, AFieldAccessInsideALambdaBodyIsRecorded) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("lambda");
    writeUnit(root, "demo/Pt.cajeta",
        "package demo;\n"
        "public class Pt {\n"
        "    public int32 x;\n"
        "    public Pt(int32 v) { this.x = v; }\n"
        "}\n");
    writeUnit(root, "demo/Plain.cajeta",
        "package demo;\n"                                          // 1
        "public class Plain {\n"                                   // 2
        "    public static int32 apply(Pt p, (Pt) -> #int32 f) {\n"// 3
        "        return 0;\n"                                      // 4
        "    }\n"                                                  // 5
        "    public void run() {\n"                                // 6
        "        Pt a #= heap Pt(3);\n"                            // 7
        "        int32 n #= Plain.apply(a, (q) -> q.x);\n"         // 8
        "    }\n"                                                  // 9
        "}\n");                                                    // 10

    bool sawInLambda = false;
    for (const auto& r : relation(lintRoot(root), "references")) {
        if (!has(r, "\"kind\": \"field\"")) continue;
        if (strField(r, "target") != "demo.Pt.x") continue;
        if (!has(strField(r, "file"), "Plain.cajeta")) continue;
        if (intField(r, "line") == 8) sawInLambda = true;
    }
    EXPECT_TRUE(sawInLambda)
        << "`(q) -> q.x` emitted no field reference. A bare-identifier lambda "
           "has no declared parameter types, so its body resolves only if the "
           "enclosing call's formal is pinned onto it first — which requires "
           "resolving the callee BEFORE its arguments.";
}

// ── 5.1.4 — a CHAINED generic receiver ────────────────────────────────────
//
// `pts.stream().fold<int32>(0, (acc, p) -> acc + p.x)` — the tour's own shape,
// and the last field reference the build found that lint did not. Two distinct
// causes, both measured rather than guessed (the plan's original hypothesis,
// "the receiver type is an unsubstituted template return", was WRONG — the
// receiver resolves to `ArrayStream<Point>` correctly):
//
//   1. `fold` is declared on the PARENT `Stream<T>`, so the shallow peek (which
//      scans only the receiver's own methodList) missed it, and the hierarchy
//      walk that finds it ran only AFTER arguments resolved — while the lambda
//      argument could not resolve until the callee was known. A deadlock.
//   2. Even once resolved, the edge was dropped: `templateKeyFor` assumed a
//      non-static method's parameter list includes the receiver, which a
//      generic METHOD's does not, so the key came back empty for `fold`, `map`
//      and `collect` while `filter` and `forEach` worked.
TEST(XrefLintContract, AChainedGenericReceiverResolvesThroughToTheLambdaBody) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("chain");
    writeUnit(root, "demo/Pt.cajeta",
        "package demo;\n"
        "public class Pt {\n"
        "    public int32 x;\n"
        "    public Pt(int32 v) { this.x = v; }\n"
        "}\n");
    writeUnit(root, "demo/Chain.cajeta",
        "package demo;\n"                                          // 1
        "import cajeta.collection.ArrayList;\n"                    // 2
        "public class Chain {\n"                                   // 3
        "    public void run() {\n"                                // 4
        "        ArrayList<Pt> pts #= heap ArrayList<Pt>();\n"     // 5
        "        int32 a #= pts.stream().fold<int32>(0,\n"         // 6
        "            (acc, p) -> acc + p.x);\n"                    // 7
        "    }\n"                                                  // 8
        "}\n");                                                    // 9

    const std::string doc = lintRoot(root);
    ASSERT_FALSE(doc.empty());

    bool sawFieldInLambda = false;
    for (const auto& r : relation(doc, "references")) {
        if (!has(r, "\"kind\": \"field\"")) continue;
        if (strField(r, "target") == "demo.Pt.x"
            && has(strField(r, "file"), "Chain.cajeta")) sawFieldInLambda = true;
    }
    EXPECT_TRUE(sawFieldInLambda)
        << "`p.x` inside the lambda emitted no field reference — the chain "
           "pts.stream() -> fold(...) -> lambda formal did not resolve through";

    bool sawFoldEdge = false;
    for (const auto& c : relation(doc, "calls"))
        if (has(strField(c, "callee"), "::fold")) sawFoldEdge = true;
    EXPECT_TRUE(sawFoldEdge)
        << "no call edge for `fold` — it resolves (the field reference above "
           "proves it) but templateKeyFor returned an empty key, and an empty "
           "key is dropped silently";
}

// ── 5.1.7 — a stream chain rooted on an ARRAY resolves ────────────────────
//
// `int32[] xs; xs.stream().filter(...)...` — the dominant residual after
// 5.1.6, measured: the tour's stream demos run over ARRAYS, and `arr.stream()`
// is not a method at all. Codegen lowers it inline to
// `heap ArrayStream<T>(arr, count)` (P6.6), whose ctor resolution records the
// ArrayStream constructor edge at the user's `stream()` position. Lint's
// receiver resolver sees a CajetaArray, not a class, finds no callee, and the
// ENTIRE chain downstream is dead — which is where most of the 134 build-only
// edges came from (StreamsDemo, ParallelStreamsDemo, CollectorsDemo).
//
// The fix mirrors the same resolution under lint: instantiate ArrayStream<T>,
// record the same ctor edge through the same choke point, and pin the return
// type so the chain continues.
TEST(XrefLintContract, AStreamChainRootedOnAnArrayResolves) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("arrstream");
    writeUnit(root, "demo/Arr.cajeta",
        "package demo;\n"                                            // 1
        "public class Arr {\n"                                       // 2
        "    public void run() {\n"                                  // 3
        "        int32[] xs = [1, 2, 3, 4];\n"                       // 4
        "        int32 evens = xs.stream()\n"                        // 5
        "            .filter((x) -> x % 2 == 0)\n"                   // 6
        "            .count();\n"                                    // 7
        "    }\n"                                                    // 8
        "}\n");                                                      // 9

    std::vector<std::string> callees;
    for (const auto& c : relation(lintRoot(root), "calls"))
        if (has(strField(c, "file"), "Arr.cajeta"))
            callees.push_back(strField(c, "callee"));

    auto saw = [&](const std::string& frag) {
        for (const auto& c : callees) if (has(c, frag)) return true;
        return false;
    };

    // The root: the same ctor edge the build's lowering records, at the
    // user's own `stream()` — the target Ctrl-click gets, since no `stream`
    // method exists anywhere to open.
    EXPECT_TRUE(saw("ArrayStream::ArrayStream("))
        << "`xs.stream()` on an array produced no edge — the intrinsic "
           "resolves only in codegen's lowering, which lint never runs";
    // The chain past the root. Before the fix these die not because filter
    // or count fail, but because the RECEIVER type was never established.
    EXPECT_TRUE(saw("::filter("))
        << "the chain died at the array root — filter has no receiver type";
    EXPECT_TRUE(saw("::count("))
        << "the chain died before its terminal count()";
}

// ── 5.1.7b — a static generic call on the OPEN template ───────────────────
//
// `Tensor.zeros<float32>(s8)` — 49 of the residual edges, the largest block
// after the array-stream root. The receiver identifier resolves to the OPEN
// template class, which holds NO Method objects at all: a template's body
// walk is skipped at parse time and its members live only in the
// template-member table (that is why templateKeyFor exists). So name+arity
// matching can never succeed on it, the callee is never found, and the edge —
// plus everything chained after it — is missing.
//
// The explicit type args name a class INSTANTIATION, and an instantiation is
// a real class with real methods. Lint already materializes instantiations
// (ArrayList<int32> resolves fine); the gap was only that a bare
// `Template.method<Args>(...)` receiver never took that step.
TEST(XrefLintContract, AStaticGenericCallOnTheOpenTemplateResolves) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("opentmpl");
    writeUnit(root, "demo/Box.cajeta",
        "package demo;\n"                                            // 1
        "public class Box<E> {\n"                                    // 2
        "    int32 n;\n"                                             // 3
        "    public static #Box<E> make(int32 count) {\n"            // 4
        "        return heap Box<E>();\n"                            // 5
        "    }\n"                                                    // 6
        "    public int32 size() { return this.n; }\n"               // 7
        "}\n");                                                      // 8
    writeUnit(root, "demo/Use.cajeta",
        "package demo;\n"                                            // 1
        "public class Use {\n"                                       // 2
        "    public void run() {\n"                                  // 3
        "        int32 k = Box.make<int32>(3).size();\n"             // 4
        "    }\n"                                                    // 5
        "}\n");                                                      // 6

    std::vector<std::string> callees;
    for (const auto& c : relation(lintRoot(root), "calls"))
        if (has(strField(c, "file"), "Use.cajeta"))
            callees.push_back(strField(c, "callee"));

    bool sawMake = false, sawSize = false, sawMono = false;
    for (const auto& c : callees) {
        if (has(c, "::make(")) {
            sawMake = true;
            // Named by the TEMPLATE member key, never the instantiation —
            // the same rule every generic edge follows (spec 2.2.3).
            if (has(c, "<")) sawMono = true;
        }
        if (has(c, "::size(")) sawSize = true;
    }
    EXPECT_TRUE(sawMake)
        << "`Box.make<int32>(3)` produced no edge — the receiver resolves to "
           "the OPEN template, which holds no Method objects to match against";
    EXPECT_FALSE(sawMono) << "the edge names a monomorphized instantiation";
    EXPECT_TRUE(sawSize)
        << "the chained `.size()` produced no edge — make's return type never "
           "closed, so the chain died at the static factory";
}

// ── 5.1.7c — overloads differing by ONE parameter must not swap keys ──────
//
// A wrong-edge regression INTRODUCED by 5.1.4's two-pass arity heuristic in
// templateKeyFor, caught by the tour diff: the source calls the 3-arg
// `fold<int64>(seed, fn, combiner)`, the build records the 3-arg key, and
// lint recorded the 2-ARG key — because a lint-resolved template method's
// parameter list does not carry `this` while a build-resolved instantiation's
// does, and the receiver-counted pass matched the smaller overload (2+1==3)
// before the declared-counted pass could match the right one (3==3). The
// heuristic is ambiguous precisely when overloads differ by one parameter.
//
// The fix removes the guessing entirely: the caller computes the DECLARED
// parameter count from the resolved method itself and the lookup matches on
// that alone. Same call, one key, both paths.
TEST(XrefLintContract, OverloadsDifferingByOneParameterDoNotSwapKeys) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("offbyone");
    // METHOD templates specifically — `tally<R>`, mirroring `Stream::fold<R>`.
    // A plain method's lint-resolved parameter list carries `this` and the
    // old heuristic got it right; a METHOD TEMPLATE's does not, and that is
    // the asymmetry that swapped the keys. A first draft of this corpus used
    // plain methods and passed against the broken code.
    writeUnit(root, "demo/Bag.cajeta",
        "package demo;\n"                                            // 1
        "public class Bag<T> {\n"                                    // 2
        "    public final R tally<R>(R seed, (R, T) -> #R fn) {\n"         // 3
        "        return seed;\n"                                     // 4
        "    }\n"                                                    // 5
        "    public final R tally<R>(R seed, (R, T) -> #R fn,\n"           // 6
        "                      (R, R) -> #R combine) {\n"            // 7
        "        return seed;\n"                                     // 8
        "    }\n"                                                    // 9
        "}\n");                                                      // 10
    writeUnit(root, "demo/Use.cajeta",
        "package demo;\n"                                            // 1
        "public class Use {\n"                                       // 2
        "    public void run(Bag<int32> b) {\n"                      // 3
        "        int32 two = b.tally<int32>(0, (a, x) -> a + x);\n"  // 4
        "        int32 three = b.tally<int32>(0, (a, x) -> a + x,\n" // 5
        "                                     (l, r) -> l + r);\n"   // 6
        "    }\n"                                                    // 7
        "}\n");                                                      // 8

    std::string at4, at5;
    for (const auto& c : relation(lintRoot(root), "calls")) {
        if (!has(strField(c, "file"), "Use.cajeta")) continue;
        if (!has(strField(c, "callee"), "::tally")) continue;
        if (intField(c, "line") == 4) at4 = strField(c, "callee");
        if (intField(c, "line") == 5) at5 = strField(c, "callee");
    }
    ASSERT_FALSE(at4.empty()) << "the 2-arg call recorded no edge";
    ASSERT_FALSE(at5.empty()) << "the 3-arg call recorded no edge";
    // Count the formals in each key: the 3-arg call's key must carry one more
    // comma-separated parameter than the 2-arg call's.
    auto commas = [](const std::string& k) {
        int depth = 0, n = 0;
        auto open = k.find('(');
        for (size_t i = open + 1; i < k.size(); i++) {
            if (k[i] == '(') depth++;
            else if (k[i] == ')') { if (depth == 0) break; depth--; }
            else if (k[i] == ',' && depth == 0) n++;
        }
        return n;
    };
    EXPECT_EQ(commas(at5), commas(at4) + 1)
        << "the two call sites' keys do not differ by one parameter — an "
           "overload was swapped. line 4: " << at4 << "  line 5: " << at5;
}

// ── 5.1.6 — a chain through a METHOD-level type argument ──────────────────
//
// `xs.stream().map<int64>(fn).reduce(...)`. A generic method's return type
// arrives with its METHOD type parameters unresolved, because only CLASS
// parameters are substituted at instantiation. Measured, on the same chain:
//
//   filter -> Stream<T>  (CLASS param)  => Stream<int32>, CLOSED, targs=1
//   map<R> -> Stream<R>  (METHOD param) => Stream,        OPEN,   targs=0
//
// So an OPEN template return identifies the method-parameter case exactly —
// it is not ambiguous with the class-parameter one, which never arrives open.
// That invariant is what makes rebinding sound rather than a guess, and this
// test pins BOTH halves of it: the chain resolves through `map<R>`, and the
// `filter` link that returns the class's own parameter still resolves too.
TEST(XrefLintContract, AChainThroughAMethodTypeArgumentResolves) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("mtarg");
    writeUnit(root, "demo/Chain.cajeta",
        "package demo;\n"                                            // 1
        "import cajeta.collection.ArrayList;\n"                      // 2
        "public class Chain {\n"                                     // 3
        "    public void run() {\n"                                  // 4
        "        ArrayList<int32> xs #= heap ArrayList<int32>();\n"   // 5
        "        int64 t #= xs.stream()\n"                           // 6
        "            .filter((x) -> x > 0)\n"                        // 7
        "            .map<int64>((x) -> (int64) x)\n"                // 8
        "            .reduce(0L, (a, b) -> a + b);\n"                // 9
        "    }\n"                                                    // 10
        "}\n");                                                      // 11

    std::vector<std::string> callees;
    for (const auto& c : relation(lintRoot(root), "calls"))
        if (has(strField(c, "file"), "Chain.cajeta"))
            callees.push_back(strField(c, "callee"));

    auto sawMethod = [&](const std::string& name) {
        for (const auto& c : callees)
            if (has(c, "::" + name + "(") || has(c, "::" + name + "()")) return true;
        return false;
    };

    // The links before the method-type-argument one already worked; they are
    // here so a regression in them is not mistaken for this feature failing.
    EXPECT_TRUE(sawMethod("stream")) << "the chain did not start";
    EXPECT_TRUE(sawMethod("filter"))
        << "a CLASS-parameter return (Stream<T>) stopped resolving";
    EXPECT_TRUE(sawMethod("map"))   << "map<int64> itself did not resolve";

    // The one this test exists for: everything AFTER a method-type-argument
    // call. Without rebinding, `map<R>`'s return is the open template `Stream`
    // and `reduce`'s receiver resolves to nothing.
    EXPECT_TRUE(sawMethod("reduce"))
        << "no call edge for `reduce` — the receiver is `map<int64>`'s return, "
           "which arrives as the OPEN template `Stream` with R unbound, so the "
           "chain stops one link short of the end";
}

// ── 5.1.5 — an advisory resolve must not invent a second call edge ────────
//
// `CajetaClass::resolveMethod` is the xref recording choke point, so ANY
// resolution during codegen writes an edge at whatever call site is open —
// including the advisory throws-lint resolve inside
// MethodCallExpression::generateCode. That probe did not thread the call's
// explicit method type-args, so a templated call resolved against the OTHER
// same-name overload and the wrong answer landed in the index as a second edge
// at the user's line, naming an overload the source never calls.
//
// Two properties are asserted together, because fixing either alone is wrong:
// the bad edge is gone, AND the site still has its real edge. Masking the probe
// out of the index removes both — measured: it took 71 legitimate edges with it
// over samples/tour, being their only recorder.
TEST(XrefLintContract, AnAdvisoryResolveDoesNotInventASecondCallEdge) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("advisory");
    writeUnit(root, "demo/Pick.cajeta",
        "package demo;\n"
        "public class Pick {\n"
        "    public static int32 at<T>(T[] a, int32 n, T key,\n"
        "                              (T, T) -> int32 cmp) {\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 at<T>(T[] a, int32 n, T key) {\n"
        "        return at<T>(a, n, key, (x, y) -> { return 0; });\n"
        "    }\n"
        "}\n");
    // The comparator overload must ALREADY be instantiated when line 6
    // resolves — that is the trigger, and without line 5 this reproduces
    // nothing. `SortDemo` in samples/tour has exactly this shape: it calls the
    // comparator forms of sort/binarySearch before reaching the 3-arg
    // lowerBound. A first attempt at this test omitted the prior call and
    // passed against the BROKEN compiler, which is the only reason this
    // comment exists.
    writeUnit(root, "demo/Main.cajeta",
        "package demo;\n"                                             // 1
        "public class Main {\n"                                       // 2
        "    public static int32 run() {\n"                           // 3
        "        int32[] xs = stack int32[4];\n"                      // 4
        "        int32 a = Pick.at<int32>(xs, 4, 2, (x, y) -> { return 0; });\n" // 5
        "        int32 b = Pick.at<int32>(xs, 4, 2);\n"               // 6
        "        return a + b;\n"                                     // 7
        "    }\n"                                                     // 8
        "}\n");                                                       // 9

    const std::string doc = buildExport(root, "demo.Main.run");
    ASSERT_FALSE(doc.empty()) << "the build export is empty";

    std::vector<std::string> atLine6;
    for (const auto& c : relation(doc, "calls")) {
        if (!has(strField(c, "file"), "Main.cajeta")) continue;
        if (intField(c, "line") != 6) continue;
        if (has(strField(c, "callee"), "::at")) atLine6.push_back(strField(c, "callee"));
    }

    ASSERT_EQ(atLine6.size(), 1u)
        << "line 6 calls ONE overload of `at`; the export carries "
        << atLine6.size() << " edge(s) for it. A second edge here names an "
           "overload the source never calls and sends \"who calls this\" to the "
           "wrong declaration.";
    EXPECT_FALSE(has(atLine6[0], "->"))
        << "the edge names the 4-arg comparator overload, not the 3-arg form "
           "actually called: " << atLine6[0];

    // The real edge is still there. Masking the advisory resolve out of the
    // index would also satisfy the assertions above while destroying 71
    // legitimate edges over samples/tour — measured.
    bool sawComparatorCallOnItsOwnLine = false;
    for (const auto& c : relation(doc, "calls"))
        if (has(strField(c, "file"), "Main.cajeta") && intField(c, "line") == 5
            && has(strField(c, "callee"), "::at")) sawComparatorCallOnItsOwnLine = true;
    EXPECT_TRUE(sawComparatorCallOnItsOwnLine)
        << "line 5's own call lost its edge — the advisory resolve must keep "
           "recording, it is the only recorder for many real edges";
}

// ── 5.1.3 — a generic METHOD on a NON-generic owner ───────────────────────
//
// `templateKeyFor` is applied only when the OWNER canonical carries `<`, so a
// generic method on a plain class keeps its monomorphized key on the build
// path (`first(int32[])`) while lint names the template (`first(T[])`). Each
// export is self-consistent, so nothing dangles — but the two paths must not
// name the same call site differently, or an index merged from both fragments
// "who calls first".
TEST(XrefLintContract, AGenericMethodOnAPlainOwnerNamesTheSameCalleeBothWays) {
    if (!haveCompiler()) GTEST_SKIP() << "compiler binary not built";
    auto root = freshTempDir("genmethod");
    writeUnit(root, "demo/Pick.cajeta",
        "package demo;\n"
        "public class Pick {\n"
        "    public static T first<T>(T[] xs) { return xs[0]; }\n"
        "}\n");
    writeUnit(root, "demo/Main.cajeta",
        "package demo;\n"                                   // 1
        "public class Main {\n"                             // 2
        "    public static int32 run() {\n"                 // 3
        "        int32[] a = stack int32[2];\n"             // 4
        "        return Pick.first<int32>(a);\n"            // 5
        "    }\n"                                           // 6
        "}\n");                                             // 7

    auto calleeAt5 = [](const std::string& doc) {
        for (const auto& c : relation(doc, "calls")) {
            if (!has(strField(c, "file"), "Main.cajeta")) continue;
            if (intField(c, "line") != 5) continue;
            if (has(strField(c, "callee"), "first")) return strField(c, "callee");
        }
        return std::string();
    };

    const std::string lintKey  = calleeAt5(lintRoot(root));
    const std::string buildKey = calleeAt5(buildExport(root, "demo.Main.run"));

    ASSERT_FALSE(lintKey.empty())  << "lint recorded no call edge for Pick.first";
    ASSERT_FALSE(buildKey.empty()) << "build recorded no call edge for Pick.first";
    EXPECT_EQ(lintKey, buildKey)
        << "the two paths name the same call site differently — lint '"
        << lintKey << "' vs build '" << buildKey << "'. templateKeyFor maps an "
           "instantiation back to its template member only when the OWNER is "
           "generic; a generic METHOD on a plain owner needs the same mapping.";
}
