//
// Unit 1 of the `record` feature (docs/specification/nucleo/records-spec.md):
// `record Name { ... }` parses and lowers to a @ValueType final class with no
// vtable. Construction is named aggregate-init; access/copy/equality follow
// value semantics; template records monomorphize; nested records are value
// sub-aggregates. Plan: agents/cajeta/nucleo/records-plan.md §1.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "cajeta/type/CajetaClass.h"

#include "llvm/IR/DerivedTypes.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

cajeta::CajetaClassPtr classOf(const std::string& canonical) {
    auto& cmap = cajeta::CajetaType::getCanonicalMap();
    auto it = cmap.find(canonical);
    if (it == cmap.end()) return nullptr;
    return std::dynamic_pointer_cast<cajeta::CajetaClass>(it->second);
}

const char* kPointSrc =
    "package test;\n"
    "public record Point {\n"
    "    float64 x;\n"
    "    float64 y;\n"
    "}\n";

} // namespace

// 1.1.1 — `record Point { float64 x; float64 y; }` parses; Point is a usable
// named type with two typed fields in declared order.
TEST(RecordTests, recordParsesWithOrderedTypedFields) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.5, y: 2.25 };\n"
        "        return (int32)(p.x * 10.0 + p.y * 100.0);\n"  // 15 + 225
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 240);

    auto cls = classOf("test.Point");
    ASSERT_NE(cls, nullptr);
    EXPECT_TRUE(cls->isValueType());
    EXPECT_TRUE(cls->getTypeFlags() & VALUE_TYPE_FLAG);
    EXPECT_TRUE(cls->getTypeFlags() & BY_VALUE_FLAG);
    std::vector<std::string> names;
    for (auto& p : cls->getPropertyList()) {
        if (p && !p->isStatic()) names.push_back(p->getName());
    }
    ASSERT_EQ(names.size(), 2u);
    EXPECT_EQ(names[0], "x");
    EXPECT_EQ(names[1], "y");
}

// 1.1.2 — reading an undeclared field is a compile error.
TEST(RecordCompileErrorTests, unknownFieldReadRejected) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.0, y: 2.0 };\n"
        "        return (int32) p.z;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 1.1.2 — an undeclared field label in the initializer is a compile error.
TEST(RecordCompileErrorTests, unknownFieldInInitializerRejected) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.0, z: 2.0 };\n"
        "        return (int32) p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 1.1.3 — assignment copies by value: mutating the copy leaves the source
// unchanged. (Unit 3 makes direct field writes a compile error; this test
// then moves to `with(...)`.)
TEST(RecordTests, assignmentCopiesByValue) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point a = Point { x: 1.0, y: 2.0 };\n"
        "        Point b = a;\n"
        "        b.x = 9.0;\n"
        "        return (int32)(a.x * 10.0 + b.x);\n"  // copy => 19, alias => 99
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 19);
}

// 1.1.3 — structural equality: equal-field records compare equal, no
// reference identity.
TEST(RecordTests, structuralEquality) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point a = Point { x: 1.0, y: 2.0 };\n"
        "        Point b = Point { x: 1.0, y: 2.0 };\n"
        "        Point c = Point { x: 3.0, y: 2.0 };\n"
        "        int32 r = 0;\n"
        "        if (a == b) { r = r + 1; }\n"
        "        if (a == c) { r = r + 10; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.4 / 1.3.1 — no per-instance header: the record's LLVM struct is just
// the field slots (no vtable pointer at slot 0), so offset-0 is the first
// field and the value size is the sum of the field sizes.
TEST(RecordTests, layoutHasNoPerInstanceHeader) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto cls = classOf("test.Point");
    ASSERT_NE(cls, nullptr);
    EXPECT_FALSE(cls->hasVtablePointerAtSlotZero());
    auto* st = llvm::dyn_cast<llvm::StructType>(cls->getLlvmType());
    ASSERT_NE(st, nullptr);
    ASSERT_EQ(st->getNumElements(), 2u);
    EXPECT_TRUE(st->getElementType(0)->isDoubleTy());
    EXPECT_TRUE(st->getElementType(1)->isDoubleTy());
}

// 1.1.4 — cajeta-side reflection agrees: first field at offset 0, two fields,
// instance size == sum of field sizes (16). The class literal is the record's
// reflection entry point — `Class.of(instance)` reads a header records don't
// have.
TEST(RecordTests, reflectionFieldOffsetZero) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Class<Point> c = Point.class;\n"
        "        int32 off0 = c.getFieldOffset(0);\n"
        "        int32 n = c.getFieldCount();\n"
        "        int32 size = (int32) c.getInstanceSize();\n"
        "        return off0 * 1000 + size * 10 + n;\n"  // 0 + 160 + 2
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 162);
}

// 1.3.1 — the record VALUE in emitted IR is the bare field-bytes struct: no
// vtable/header slot. (A module-level `#VTable`/RTTI global still exists —
// that is the Class<Point> reflection surface, not per-instance state.)
TEST(RecordTests, emittedIrHasNoVtableSlot) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.0, y: 2.0 };\n"
        "        return (int32) p.x;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    const std::string& ir = jit->getModuleIr();
    ASSERT_FALSE(ir.empty());
    EXPECT_NE(ir.find("%test.Point = type { double, double }"),
              std::string::npos);
    EXPECT_NE(ir.find("alloca %test.Point"), std::string::npos);
}

// 1.1.5 — a template record monomorphizes per type-arg list; every
// instantiation is a value type.
TEST(RecordTests, templateRecordMonomorphizes) {
    auto src =
        "package test;\n"
        "public record RecPair<A, B> {\n"
        "    A first;\n"
        "    B second;\n"
        "    public RecPair(A f, B s) { this.first = f; this.second = s; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RecPair<int32, int32> p = heap RecPair<int32, int32>(3, 4);\n"
        "        RecPair<float64, float64> q = heap RecPair<float64, float64>(0.5, 0.25);\n"
        "        return p.first * 100 + p.second * 10 + (int32)(q.first * 4.0 + q.second * 8.0);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 344);

    int valueTypedInstantiations = 0;
    for (auto& kv : cajeta::CajetaType::getCanonicalMap()) {
        if (kv.first.rfind("test.RecPair<", 0) != 0 || !kv.second) continue;
        if (kv.second->getTypeFlags() & VALUE_TYPE_FLAG) valueTypedInstantiations++;
    }
    EXPECT_GE(valueTypedInstantiations, 2);
}

// 1.1.6 — a record field whose type is another record embeds as a value
// sub-aggregate (inline struct, not a pointer).
TEST(RecordTests, nestedRecordFieldIsValueSubAggregate) {
    auto src =
        "package test;\n"
        "public record Inner {\n"
        "    float64 a;\n"
        "    float64 b;\n"
        "}\n"
        "public record Outer {\n"
        "    Inner i;\n"
        "    float64 c;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Outer o = Outer { i: Inner { a: 1.0, b: 2.0 }, c: 3.0 };\n"
        "        return (int32)(o.i.a + o.i.b * 10.0 + o.c * 100.0);\n"  // 321
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 321);

    auto outer = classOf("test.Outer");
    ASSERT_NE(outer, nullptr);
    auto* st = llvm::dyn_cast<llvm::StructType>(outer->getLlvmType());
    ASSERT_NE(st, nullptr);
    ASSERT_EQ(st->getNumElements(), 2u);
    // The inline sub-aggregate may lower as a named or literal struct;
    // shape is the contract: two doubles, no pointer indirection.
    auto* inner = llvm::dyn_cast<llvm::StructType>(st->getElementType(0));
    ASSERT_NE(inner, nullptr);
    ASSERT_EQ(inner->getNumElements(), 2u);
    EXPECT_TRUE(inner->getElementType(0)->isDoubleTy());
    EXPECT_TRUE(inner->getElementType(1)->isDoubleTy());
    EXPECT_TRUE(st->getElementType(1)->isDoubleTy());
}
