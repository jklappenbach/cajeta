//
// Reading a value-type (record / @ValueType) that is stored INLINE — a
// value-type field, or a value-type array element — must load its body by
// value, never as a pointer. Value types carry STRUCT_FLAG and are a
// CajetaClass, so the class-ref/STRUCT_FLAG load rules in loadIfLValue and
// ReturnStatement used to load the first 8 bytes of the inline value AS a
// pointer and the consumer dereferenced them (SIGSEGV, fault addr == the
// value's bytes). Surfaced as `HashMap<K, value-type V>.get` (`return
// slots[i].val`) segfaulting on plain put/get. Four inline-read sites:
// field/element × read-into-local / return-from-method.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runSrc(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}
const char* kPre =
    "package test;\n"
    "public record Point { int32 x; int32 y; }\n"
    "@ValueType public final class Cell { public Point val; }\n";
} // namespace

// Value-type FIELD read into a local.
TEST(ValueTypeInlineReadTests, valueTypeFieldIntoLocal) {
    EXPECT_EQ(runSrc(std::string(kPre) +
        "public final class D { public static int32 run() {\n"
        "  Cell[] a = heap Cell[1]; a[0].val = Point{x:3, y:4};\n"
        "  Point p = a[0].val;\n"
        "  return p.x*10 + p.y; } }\n"), 34);
}

// Value-type FIELD returned from a method (by-value return ABI).
TEST(ValueTypeInlineReadTests, valueTypeFieldReturned) {
    EXPECT_EQ(runSrc(std::string(kPre) +
        "public final class D {\n"
        "  public static Point pick(Cell[] a) { return a[0].val; }\n"
        "  public static int32 run() {\n"
        "    Cell[] a = heap Cell[1]; a[0].val = Point{x:3, y:4};\n"
        "    Point p = pick(a); return p.x*10 + p.y; } }\n"), 34);
}

// Value-type ARRAY ELEMENT read into a local.
TEST(ValueTypeInlineReadTests, valueTypeElementIntoLocal) {
    EXPECT_EQ(runSrc(
        "package test;\n"
        "public record Point { int32 x; int32 y; }\n"
        "public final class D { public static int32 run() {\n"
        "  Point[] pts = [ {x:3,y:4}, {x:5,y:6} ];\n"
        "  Point p = pts[1];\n"
        "  return p.x*10 + p.y; } }\n"), 56);
}

// Value-type ARRAY ELEMENT returned from a method.
TEST(ValueTypeInlineReadTests, valueTypeElementReturned) {
    EXPECT_EQ(runSrc(
        "package test;\n"
        "public record Point { int32 x; int32 y; }\n"
        "public final class D {\n"
        "  public static Point pick(Point[] a) { return a[0]; }\n"
        "  public static int32 run() {\n"
        "    Point[] pts = [ {x:3,y:4} ];\n"
        "    Point p = pick(pts); return p.x*10 + p.y; } }\n"), 34);
}

// The reported case: HashMap<K, value-type V> put + get.
TEST(ValueTypeInlineReadTests, hashMapValueTypeVPutGet) {
    EXPECT_EQ(runSrc(
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public record Point { int32 x; int32 y; }\n"
        "public final class D { public static int32 run() {\n"
        "  HashMap<String,Point> m = heap HashMap<String,Point>();\n"
        "  m.put(\"o\", Point{x:3,y:4}); m.put(\"p\", Point{x:5,y:6});\n"
        "  Point a = m.get(\"o\"); Point b = m.get(\"p\");\n"
        "  return a.x*1000 + a.y*100 + b.x*10 + b.y; } }\n"), 3456);
}
