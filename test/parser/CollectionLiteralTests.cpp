//
// collection-literals Unit 1 — target-typed collection literals (list/set).
// A bare `[...]` literal against a collection-class target (e.g.
// `ArrayList<int32> xs = [1,2,3]`) lowers to a from-array constructor call
// `heap Target([...])`, reusing the array-literals target-typing machinery and
// the `(T[])` constructors. The literal's element type is taken from the
// target's type argument (so `ArrayList<int64> = [1,2,3]` widens). An array
// target keeps the array path; a class without a `(T[])` ctor errors cleanly.
// (spec §2; plan Unit 1).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>
#include <filesystem>
#include <fstream>
#include <regex>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n" + imports +
        "public final class D {\n" + body + "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}
} // namespace

// 1.1.1 — ArrayList from a bare literal: size 3, order preserved, heap default.
TEST(CollectionLiteralTests, ArrayListFromBareLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = [1, 2, 3];\n"
        "        return xs.count() * 100 + xs.get(0) * 10 + xs.get(2);\n"
        "    }\n");                                    // 3*100 + 1*10 + 3 = 313
    EXPECT_EQ(r, 313);
}

// 1.1.2 — HashSet from a bare literal: duplicates collapse to {1,2,3}.
TEST(CollectionLiteralTests, HashSetFromBareLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.HashSet;\n",
        "    public static int32 run() {\n"
        "        HashSet<int32> s = [1, 2, 2, 3];\n"
        "        int32 c = (int32) s.count();\n"          // 3 (deduped)
        "        int32 has = 0;\n"
        "        if (s.contains(1)) { has = has + 1; }\n"
        "        if (s.contains(2)) { has = has + 1; }\n"
        "        if (s.contains(3)) { has = has + 1; }\n"
        "        if (s.contains(9)) { has = has + 10; }\n"  // absent
        "        return c * 10 + has;\n"                    // 3*10 + 3 = 33
        "    }\n");
    EXPECT_EQ(r, 33);
}

// 1.1.2b — the target's type argument drives the element type: an
// `ArrayList<int64>` from `[1,2,3]` builds an `int64[]` for the ctor (widening
// past the int32 the literal would unify to on its own). plan 1.2.2.
TEST(CollectionLiteralTests, ElementTypeFromTarget) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int64> xs = [1, 2, 3];\n"
        "        int64 sum = xs.get(0) + xs.get(1) + xs.get(2);\n"
        "        return (int32) sum;\n"                     // 6
        "    }\n");
    EXPECT_EQ(r, 6);
}

// 1.1.3 — a `stack`-prefixed collection literal still produces the right
// values; the prefix carries to the collection construction (the inner array
// stays a heap ctor argument). plan 1.2.1 ("carry a stack/shared prefix
// through").
TEST(CollectionLiteralTests, StackCollectionLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = stack [1, 2, 3];\n"
        "        return xs.count() * 100 + xs.get(0) * 10 + xs.get(2);\n"
        "    }\n");                                    // 3*100 + 1*10 + 3 = 313
    EXPECT_EQ(r, 313);
}

// 1.1.4 — an array target is unchanged: `[...]` against `int32[]` stays an
// array (the class-target polymorphism doesn't hijack the array path).
TEST(CollectionLiteralTests, ArrayTargetStillArray) {
    int32_t r = runI32(
        "",
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        return xs[0] * 100 + xs[1] * 10 + xs[2];\n"
        "    }\n");                                    // 123
    EXPECT_EQ(r, 123);
}

// 1.1.4b — assignment (not just declaration) also target-types a collection:
// `xs = [...]` on an already-declared ArrayList local.
TEST(CollectionLiteralTests, AssignmentTargetsCollection) {
    int32_t r = runI32(
        "import cajeta.collection.ArrayList;\n",
        "    public static int32 run() {\n"
        "        ArrayList<int32> xs = [1];\n"
        "        xs = [4, 5, 6];\n"
        "        return xs.count() * 100 + xs.get(0) * 10 + xs.get(2);\n"
        "    }\n");                                    // 3*100 + 4*10 + 6 = 346
    EXPECT_EQ(r, 346);
}

// 1.1.5 — a class target without a `(T[])` constructor gives a compile error,
// not a silent miss. `Box<int32>` has only a `(T)` ctor.
TEST(CollectionLiteralTests, NoCollectionCtorErrors) {
    std::string src =
        "package test;\n"
        "public final class Box<T> {\n"
        "    private T v;\n"
        "    public Box(T v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = [1, 2, 3];\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// ---------------------------------------------------------------------------
// Unit 2 — type-inferred aggregate literals `{x: 1, y: 2}` (spec §4).
// The `{…}` builds an aggregate of the type demanded by context (declared /
// assigned / returned type, or an enclosing array's element type); the
// explicit `Type{…}` form is unchanged. A `{…}` with no inferable type errors.
// ---------------------------------------------------------------------------

namespace {
const char* kPointRec =
    "package test;\n"
    "public record Point {\n"
    "    int32 x;\n"
    "    int32 y;\n"
    "}\n";
} // namespace

// 2.1.1 — a bare `{…}` in a declaration infers the declared type (value-inline).
TEST(CollectionLiteralTests, AggregateFromDeclaration) {
    std::string src = std::string(kPointRec) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = { x: 3, y: 4 };\n"
        "        return p.x * 10 + p.y;\n"                  // 34
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ((jit->lookup<int32_t (*)()>("run"))(), 34);
}

// 2.1.2 — each element's aggregate type is the array's element type
// (the headline composition: `Point[] pts = [{…}, {…}]`).
TEST(CollectionLiteralTests, AggregateArrayElements) {
    std::string src = std::string(kPointRec) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point[] pts = [ {x: 1, y: 2}, {x: 3, y: 4} ];\n"
        "        return pts[0].x * 1000 + pts[0].y * 100\n"
        "             + pts[1].x * 10 + pts[1].y;\n"        // 1234
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ((jit->lookup<int32_t (*)()>("run"))(), 1234);
}

// 2.1.3 — `heap {…}` against a reference (non-value) class heap-allocates.
TEST(CollectionLiteralTests, HeapAggregate) {
    std::string src =
        "package test;\n"
        "public class RefBox {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RefBox b = heap { x: 5, y: 7 };\n"
        "        return b.x * 10 + b.y;\n"                  // 57
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ((jit->lookup<int32_t (*)()>("run"))(), 57);
}

// 2.1.4 — the explicit `Type{…}` prefix still works, unchanged.
TEST(CollectionLiteralTests, ExplicitPrefixUnchanged) {
    std::string src = std::string(kPointRec) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 8, y: 9 };\n"
        "        return p.x * 10 + p.y;\n"                  // 89
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ((jit->lookup<int32_t (*)()>("run"))(), 89);
}

// 2.1.5 — a `{…}` with no inferable target type is a compile error.
TEST(CollectionLiteralTests, NoInferableTypeErrors) {
    std::string src = std::string(kPointRec) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return { x: 1, y: 2 };\n"   // return type int32 — not a class
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// ---------------------------------------------------------------------------
// Unit 3 — map literals `[k: v]` and `[:]` (spec §3). Lowered via `Pair<K,V>[]`
// → `HashMap<K,V>(Pair<K,V>[])`. The colon is the map/sequence signal; it must
// not shadow the ternary `?:` inside an element.
// ---------------------------------------------------------------------------

// 3.1.1 — a HashMap from a map literal: the two mappings read back by key.
TEST(CollectionLiteralTests, HashMapFromLiteral) {
    int32_t r = runI32(
        "import cajeta.collection.HashMap;\n",
        "    public static int32 run() {\n"
        "        HashMap<String, int32> m = [\"blah\": 123, \"x\": 4];\n"
        "        return (int32) m.count() * 1000 + m.get(\"blah\") - m.get(\"x\");\n"
        "    }\n");                                    // 2*1000 + 123 - 4 = 2119
    EXPECT_EQ(r, 2119);
}

// 3.1.2 — `[:]` is the empty map.
TEST(CollectionLiteralTests, EmptyMap) {
    int32_t r = runI32(
        "import cajeta.collection.HashMap;\n",
        "    public static int32 run() {\n"
        "        HashMap<String, int32> e = [:];\n"
        "        return (int32) e.count();\n"          // 0
        "    }\n");
    EXPECT_EQ(r, 0);
}

// 3.1.3 — the colon is the signal: `["a": 1]` is a map, `[1,2,3]` a sequence,
// in map- and array-typed contexts respectively.
TEST(CollectionLiteralTests, MapVsSequence) {
    int32_t r = runI32(
        "import cajeta.collection.HashMap;\n",
        "    public static int32 run() {\n"
        "        HashMap<String, int32> m = [\"a\": 5];\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        return (int32) m.count() * 1000 + m.get(\"a\") * 100\n"
        "             + xs[0] * 10 + xs[2];\n"         // 1000 + 500 + 10 + 3 = 1513
        "    }\n");
    EXPECT_EQ(r, 1513);
}

// 3.1.4 — a ternary inside an element stays a one-element sequence; the
// ternary's colon is NOT a map separator.
TEST(CollectionLiteralTests, TernaryElementNotMap) {
    int32_t r = runI32(
        "",
        "    public static int32 run() {\n"
        "        int32 a = 7; int32 b = 9; boolean c = true;\n"
        "        int32[] xs = [c ? a : b, 100];\n"
        "        return xs[0] + xs[1];\n"              // 7 + 100 = 107
        "    }\n");
    EXPECT_EQ(r, 107);
}

// 3.1.5 — the value type is inferred from the map's V, composing with Unit 2:
// `HashMap<String,Point> m = ["o": {x:3, y:4}]` builds a Point (value-type V)
// from the aggregate. (Value-type V works after the inline-value-read fix —
// see ValueTypeInlineReadTests.)
TEST(CollectionLiteralTests, MapToAggregate) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public record Point {\n"
        "    int32 x;\n"
        "    int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<String, Point> m = [\"o\": {x: 3, y: 4}];\n"
        "        Point p = m.get(\"o\");\n"
        "        return p.x * 10 + p.y;\n"             // 34
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ((jit->lookup<int32_t (*)()>("run"))(), 34);
}

// ---------------------------------------------------------------------------
// Unit 4 — composition (spec §5). The three forms nest by their bracket rules;
// the target type must thread through each nesting boundary.
// ---------------------------------------------------------------------------

// 4.1.1 — collection of aggregates: `ArrayList<Box> ps = [{…}, {…}]`. Each
// element's aggregate infers Box and, as a reference-class collection element,
// is heap-allocated (it escapes into the list). Uses a REFERENCE class: the
// spec's value-type `ArrayList<Point>` (§5.2.1) is blocked by a separate
// value-type-in-generic-collections effort (see that spec) — the composition
// machinery here is identical either way.
TEST(CollectionLiteralTests, ListOfAggregates) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Box { public int32 x; public int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Box> ps = [ {x:1, y:2}, {x:3, y:4} ];\n"
        "        return ps.get(0).x * 1000 + ps.get(0).y * 100\n"
        "             + ps.get(1).x * 10 + ps.get(1).y;\n"      // 1234
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ((jit->lookup<int32_t (*)()>("run"))(), 1234);
}

// 4.1.2 — map to collections: `HashMap<String,int32[]> g = ["a":[1,2], …]`.
TEST(CollectionLiteralTests, MapToList) {
    int32_t r = runI32(
        "import cajeta.collection.HashMap;\n",
        "    public static int32 run() {\n"
        "        HashMap<String, int32[]> g = [\"a\": [1, 2], \"b\": [3, 4]];\n"
        "        int32[] av = g.get(\"a\");\n"
        "        int32[] bv = g.get(\"b\");\n"
        "        return av[0] * 1000 + av[1] * 100 + bv[0] * 10 + bv[1];\n"  // 1234
        "    }\n");
    EXPECT_EQ(r, 1234);
}

// 4.1.3 — nesting through an aggregate: `Point[][] grid = [[{…}], [{…}]]`.
TEST(CollectionLiteralTests, NestedThroughAggregate) {
    std::string src =
        "package test;\n"
        "public record Point { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point[][] grid = [ [ {x:2, y:3} ], [ {x:4, y:5} ] ];\n"
        "        return grid[0][0].x * 1000 + grid[0][0].y * 100\n"
        "             + grid[1][0].x * 10 + grid[1][0].y;\n"     // 2345
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ((jit->lookup<int32_t (*)()>("run"))(), 2345);
}

// ---- Unit 5: retire array-`{…}` (full-retirement carve-out) ----

// 5.1.1 — the array brace-initializer `{…}` is retired for value/data arrays:
// `int32[] xs = {1,2,3}` no longer compiles (positional `{…}` resolves to the
// prefixless aggregate form, which rejects a non-class array target; the
// dedicated retirement diagnostic fires from ArrayInitializer::generateCode).
// The bracket form is the replacement. Spec §6.2; plan 5.1.1.
TEST(CollectionLiteralTests, BraceArrayIsAggregateOnly) {
    std::string braced =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3};\n"
        "        return xs[1];\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(braced, "test.D"));

    // The `[…]` replacement compiles and runs.
    std::string bracketed =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = [1, 2, 3];\n"
        "        return xs[1];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(bracketed, "test.D");
    EXPECT_EQ((jit->lookup<int32_t (*)()>("run"))(), 2);
}

// 5.1.2 — no in-tree `.cajeta` uses the retired array-`{…}` form. Self-locating
// via __FILE__ (…/test/parser/CollectionLiteralTests.cpp → repo root), scans the
// shipped source (runtime/src, samples) for the declarator brace pattern
// `…]  name = {`. Function-typed dispatch tables (`((T)->R)[] ops = {…}`, carved
// out) are the only legal brace-array and never appear in these trees. Spec
// §6.2.1; plan 5.1.2.
TEST(CollectionLiteralTests, NoBraceArraysInTree) {
    namespace fs = std::filesystem;
    fs::path repoRoot = fs::path(__FILE__).parent_path()  // test/parser
                            .parent_path()                 // test
                            .parent_path();                // repo root
    ASSERT_TRUE(fs::exists(repoRoot / "runtime" / "src"))
        << "repo root not resolved from __FILE__: " << repoRoot;

    // `<...>]  name = {`  — an array declarator initialized with braces. Exclude
    // the function-typed dispatch-table carve-out (any line mentioning `->`).
    std::regex braceArray(R"(\]\s*[A-Za-z_][A-Za-z0-9_]*\s*=\s*\{)");
    std::vector<std::string> offenders;
    for (const char* sub : {"runtime/src", "samples"}) {
        fs::path root = repoRoot / sub;
        if (!fs::exists(root)) continue;
        // Never follow symlinks. `is_regular_file()` stats *through* a link, so a
        // self-referential one (e.g. local profiling output under
        // samples/profile/results/) raises ELOOP and aborts the whole scan. A
        // source scan has no reason to traverse links, so skip them outright.
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied);
        for (auto& e : it) {
            if (e.is_symlink()) continue;
            if (!e.is_regular_file() || e.path().extension() != ".cajeta") continue;
            std::ifstream in(e.path());
            std::string line;
            int n = 0;
            while (std::getline(in, line)) {
                ++n;
                if (line.find("->") != std::string::npos) continue;  // dispatch table
                if (std::regex_search(line, braceArray))
                    offenders.push_back(e.path().string() + ":" + std::to_string(n)
                                        + "  " + line);
            }
        }
    }
    EXPECT_TRUE(offenders.empty())
        << "retired array-`{…}` still in tree:\n"
        << [&] { std::string s; for (auto& o : offenders) s += "  " + o + "\n"; return s; }();
}
