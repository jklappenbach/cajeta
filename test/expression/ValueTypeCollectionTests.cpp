//
// value-type-generic-collections Unit 1 — value-type parameter field-access ABI.
//
// A value type (record / @ValueType) as the element `T` of a generic collection
// reaches a `JIT verify failed: GEP base pointer is not a vector` when the
// instantiated body (e.g. operator< / operator==, or any helper) accesses a
// field of a BY-VALUE value-type parameter: codegen GEPs the SSA struct value
// instead of a pointer. The by-value param must be materialized (spilled to an
// entry-block alloca once) before the field GEP.
//
// 1.1.3 is the reduced repro (no collection): a static method taking a value-type
// param by value and reading its fields, called with a loaded value.
// (spec §2.2; plan Unit 1).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}
} // namespace

// 1.1.3 — the reduced repro: field access on a by-value value-type parameter,
// called with a value loaded from an array element.
TEST(ValueTypeCollectionTests, staticValueTypeParamFieldAccess) {
    auto src =
        "package test;\n"
        "public record Point { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 f(Point p) { return p.x * 10 + p.y; }\n"
        "    public static int32 run() {\n"
        "        Point[] pts = [ {x:3, y:4} ];\n"
        "        return f(pts[0]);\n"          // 34
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 34);
}

// 1.1.1 — ArrayList<Point>: construct + add + get, no sort.
TEST(ValueTypeCollectionTests, arrayListValueTypeAddGet) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public record Point { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Point> ps = heap ArrayList<Point>();\n"
        "        ps.add(Point{x:1, y:2});\n"
        "        ps.add(Point{x:3, y:4});\n"
        "        Point a = ps.get(0);\n"
        "        Point b = ps.get(1);\n"
        "        return a.x * 1000 + a.y * 100 + b.x * 10 + b.y;\n"  // 1234
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1234);
}

// 1.1.2 — ArrayList<Point>: add unsorted, sort() by default lexicographic order,
// read back. Exercises the instantiated operator< over by-value value-type args.
TEST(ValueTypeCollectionTests, arrayListValueTypeSort) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public record Point { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Point> ps = heap ArrayList<Point>();\n"
        "        ps.add(Point{x:3, y:1});\n"
        "        ps.add(Point{x:1, y:9});\n"
        "        ps.add(Point{x:1, y:2});\n"
        "        ps.sort();\n"                 // lexicographic: (1,2),(1,9),(3,1)
        "        Point a = ps.get(0);\n"
        "        Point b = ps.get(1);\n"
        "        Point c = ps.get(2);\n"
        "        return a.x*100000 + a.y*10000 + b.x*1000 + b.y*100 + c.x*10 + c.y;\n"  // (1,2)(1,9)(3,1) -> 121931
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 121931);
}

// ---- Unit 2: sequence collections sweep (spec §3.1–3.3) ----

// 2.1.1 — HashSet<Point>: dedup by structural hash/==, contains.
TEST(ValueTypeCollectionTests, hashSetValueType) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "@AutoHash public record Point { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<Point> s = heap HashSet<Point>(16);\n"
        "        s.add(Point{x:1, y:2});\n"
        "        s.add(Point{x:1, y:2});\n"                 // dup
        "        s.add(Point{x:3, y:4});\n"
        "        int32 c = (int32) s.count();\n"            // 2
        "        boolean has = s.contains(Point{x:3, y:4});\n"  // true
        "        return c * 10 + (has ? 1 : 0);\n"          // 21
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 21);
}

// 2.1.2 — LinkedList<Point>: ordered store + get.
TEST(ValueTypeCollectionTests, linkedListValueType) {
    auto src =
        "package test;\n"
        "import cajeta.collection.LinkedList;\n"
        "public record Point { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        LinkedList<Point> l = heap LinkedList<Point>();\n"
        "        l.add(Point{x:1, y:2});\n"
        "        l.add(Point{x:3, y:4});\n"
        "        Point a = l.get(0);\n"
        "        Point b = l.get(1);\n"
        "        return a.x*1000 + a.y*100 + b.x*10 + b.y;\n"  // 1234
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1234);
}

// 2.1.3 — ImmutableList<Point> keeps dups; ImmutableSet<Point> dedups.
TEST(ValueTypeCollectionTests, immutableListSetValueType) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ImmutableList;\n"
        "import cajeta.collection.ImmutableSet;\n"
        "@AutoHash public record Point { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point[] src = [ Point{x:1,y:2}, Point{x:3,y:4}, Point{x:1,y:2} ];\n"
        "        ImmutableList<Point> il = heap ImmutableList<Point>(src);\n"
        "        ImmutableSet<Point> is = heap ImmutableSet<Point>(src);\n"
        "        int32 lc = (int32) il.count();\n"   // 3 (list keeps dups)
        "        int32 sc = (int32) is.count();\n"   // 2 (set dedups)
        "        return lc * 10 + sc;\n"             // 32
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 32);
}
