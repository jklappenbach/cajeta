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

// True when the IR keeps a real CALL to `symbol` (its `define` line always
// exists; only call sites matter for the inline check). Same oracle as
// ValueTypeInlineSizeTests.
bool hasCallTo(const std::string& ir, const std::string& symbol) {
    const std::string needle = "@\"" + symbol;
    size_t pos = 0;
    while ((pos = ir.find(needle, pos)) != std::string::npos) {
        size_t lineStart = ir.rfind('\n', pos);
        std::string line = ir.substr(
            lineStart == std::string::npos ? 0 : lineStart,
            pos - (lineStart == std::string::npos ? 0 : lineStart));
        if (line.find("call ") != std::string::npos) return true;
        pos += 1;
    }
    return false;
}

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
        "        a = a.with(x: 9.0);\n"  // rebind the LOCAL (fields are immutable)
        "        return (int32)(b.x * 10.0 + a.x);\n"  // copy => 19, alias => 99
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
    // No vtable-pointer store into the value either — slot 0 is field x,
    // not a header (aggregate-init must not write one).
    EXPECT_EQ(ir.find("store ptr @\"test.Point#VTable\""), std::string::npos);
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

// ---------------------------------------------------------------------
// Unit 2 — constraint gates (plan §2; spec 1.3, 2.4, 2.5.4, 2.6.4–2.6.5)
// ---------------------------------------------------------------------

// 2.1.1 — a record may carry pure methods + operator overloads; both dispatch
// directly (no per-instance vtable exists to dispatch through).
// 2.2.3 — a small record operator is force-inlined (@ValueType alwaysinline
// path): no CALL to it survives in the O0 post-AlwaysInline IR.
TEST(RecordTests, methodsAndOperatorsDispatchDirectly) {
    auto src =
        "package test;\n"
        "public record RVec {\n"
        "    float64 x;\n"
        "    float64 y;\n"
        "    public float64 length2() { return this.x * this.x + this.y * this.y; }\n"
        "    public static RVec operator+ (RVec a, RVec b) {\n"
        "        return RVec { x: a.x + b.x, y: a.y + b.y };\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RVec a = RVec { x: 1.0, y: 2.0 };\n"
        "        RVec b = RVec { x: 3.0, y: 4.0 };\n"
        "        RVec c = a + b;\n"
        "        return (int32) c.length2();\n"  // 16 + 36 = 52
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 52);
    EXPECT_FALSE(hasCallTo(jit->getModuleIr(), "test.RVec::operator+"));
}

// 2.1.3 — a @ValueType class field is allowed in a record (value aggregates
// compose); one more leaf of the value-only field rule.
TEST(RecordTests, valueTypeClassFieldAllowed) {
    auto src =
        "package test;\n"
        "@ValueType public final class Vt2 {\n"
        "    public float64 a;\n"
        "    public float64 b;\n"
        "}\n"
        "public record RVt {\n"
        "    Vt2 v;\n"
        "    float64 c;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RVt r = RVt { v: Vt2 { a: 1.0, b: 2.0 }, c: 3.0 };\n"
        "        return (int32)(r.v.a + r.v.b * 10.0 + r.c * 100.0);\n"  // 321
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 321);
}

// 2.1.4 — one-way containment: a (reference) class may hold a record field.
TEST(RecordTests, classContainingRecordFieldAllowed) {
    auto src = std::string(kPointSrc) +
        "public class Holder {\n"
        "    Point p;\n"
        "    public Holder() {\n"
        "        this.p = Point { x: 1.5, y: 2.5 };\n"
        "    }\n"
        "    public float64 sum() { return this.p.x + this.p.y; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        return (int32)(h.sum() * 10.0);\n"  // 40
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 40);
}

// 2.1.2 — a body-less (abstract/virtual) method on a record is a compile
// error: records have no vtable, every method needs a body.
TEST(RecordCompileErrorTests, abstractMethodRejected) {
    auto src =
        "package test;\n"
        "public record RAbs {\n"
        "    float64 w;\n"
        "    public float64 area();\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 2.1.2 — `implements <Interface>` on a record is a compile error (parsed for
// the diagnostic, rejected in the visitor).
TEST(RecordCompileErrorTests, implementsInterfaceRejected) {
    auto src =
        "package test;\n"
        "public interface Tagged {\n"
        "    public int32 tag();\n"
        "}\n"
        "public record RIface implements Tagged {\n"
        "    int32 a;\n"
        "    public int32 tag() { return this.a; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 2.1.2 — an `abstract` record declaration is a compile error.
TEST(RecordCompileErrorTests, abstractRecordRejected) {
    auto src =
        "package test;\n"
        "public abstract record RA {\n"
        "    int32 a;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 2.1.3 — a reference-type (heap class) field in a record is a compile error.
TEST(RecordCompileErrorTests, referenceClassFieldRejected) {
    auto src =
        "package test;\n"
        "public class RefBox {\n"
        "    public int32 a;\n"
        "}\n"
        "public record RBad {\n"
        "    RefBox b;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 2.1.3 — String is a heap class: a String record field is rejected (the
// future Utf8 value type is the blessed text field; no special case).
TEST(RecordCompileErrorTests, stringFieldRejected) {
    auto src =
        "package test;\n"
        "public record RStr {\n"
        "    String s;\n"
        "    int32 id;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 2.1.3 corollary — a record binding into another record COPIES (value
// semantics): the source local stays live and readable after the init (no
// use-after-move; reference-class bindings DO move).
TEST(RecordTests, recordBindingCopiesSourceStaysLive) {
    auto src =
        "package test;\n"
        "public record RInner {\n"
        "    float64 a;\n"
        "    float64 b;\n"
        "}\n"
        "public record ROuter {\n"
        "    RInner i;\n"
        "    float64 c;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RInner q = RInner { a: 1.0, b: 2.0 };\n"
        "        ROuter o = ROuter { i: q, c: 3.0 };\n"
        "        return (int32)(o.i.a + o.i.b * 10.0 + o.c * 100.0 + q.a * 1000.0);\n"  // 1321
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1321);
}

// ---------------------------------------------------------------------
// Unit 3 — immutability + copy-with (plan §3; spec 3.2, 3.3)
// ---------------------------------------------------------------------

// 3.1.1 — a field write on a (default-immutable) record is a compile error.
TEST(RecordCompileErrorTests, fieldWriteRejected) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.0, y: 2.0 };\n"
        "        p.x = 9.0;\n"
        "        return (int32) p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 3.1.1 — a record's own non-constructor method may not mutate `this` either.
TEST(RecordCompileErrorTests, methodSelfWriteRejected) {
    auto src =
        "package test;\n"
        "public record RMut {\n"
        "    float64 x;\n"
        "    public void bump() { this.x = this.x + 1.0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 3.1.1 corollary — a record CONSTRUCTOR still initializes its own fields.
TEST(RecordTests, ctorInitializesImmutableFields) {
    auto src =
        "package test;\n"
        "public record RCtor {\n"
        "    float64 x;\n"
        "    float64 y;\n"
        "    public RCtor(float64 x, float64 y) { this.x = x; this.y = y; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RCtor c = heap RCtor(1.5, 2.5);\n"
        "        return (int32)(c.x * 10.0 + c.y * 100.0);\n"  // 15 + 250
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 265);
}

// 3.1.1 corollary — a CLASS field that holds a record stays assignable
// (the receiver is the class; the record value is replaced wholesale).
TEST(RecordTests, classFieldHoldingRecordReassignable) {
    auto src = std::string(kPointSrc) +
        "public class RHolder {\n"
        "    Point p;\n"
        "    public RHolder() { this.p = Point { x: 1.0, y: 2.0 }; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        RHolder h = heap RHolder();\n"
        "        h.p = Point { x: 4.0, y: 8.0 };\n"
        "        return (int32)(h.p.x * 10.0 + h.p.y);\n"  // 48
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 48);
}

// 3.1.2 — with(field: v) yields a NEW record: one field replaced, the rest
// copied, the original untouched.
TEST(RecordTests, withReplacesOneFieldCopiesRest) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.5, y: 2.5 };\n"
        "        Point q = p.with(x: 4.5);\n"
        "        return (int32)(q.x * 10.0 + q.y * 100.0 + p.x * 1000.0);\n"  // 45+250+1500
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1795);
}

// 3.3.1 — with round-trips field values exactly: replacing a field with the
// original value compares structurally equal; multi-field with works.
TEST(RecordTests, withRoundTripsAndMultiField) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.25, y: 2.75 };\n"
        "        Point same = p.with(x: p.x);\n"
        "        Point both = p.with(x: 9.0, y: 8.0);\n"
        "        int32 r = 0;\n"
        "        if (same == p) { r = r + 1; }\n"
        "        if (both == p) { r = r + 10; }\n"
        "        r = r + (int32)(both.x + both.y * 10.0);\n"  // 9 + 80
        "        return r;\n"  // 1 + 0 + 89
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 90);
}

// 3.1.2 — with() on a template-record instantiation.
TEST(RecordTests, withOnTemplateRecordInstantiation) {
    auto src =
        "package test;\n"
        "public record WPair<A, B> {\n"
        "    A first;\n"
        "    B second;\n"
        "    public WPair(A f, B s) { this.first = f; this.second = s; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        WPair<int32, int32> p = heap WPair<int32, int32>(3, 4);\n"
        "        WPair<int32, int32> q = p.with(second: 9);\n"
        "        return q.first * 100 + q.second * 10 + p.second;\n"  // 300+90+4
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 394);
}

// 3.1.2 — an unknown field label in with(...) is a compile error.
TEST(RecordCompileErrorTests, withUnknownFieldRejected) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.0, y: 2.0 };\n"
        "        Point q = p.with(z: 3.0);\n"
        "        return (int32) q.x;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// ---------------------------------------------------------------------
// Unit 4 — static non-virtual inheritance (plan §4; spec 2.6.1–2.6.3)
// ---------------------------------------------------------------------

namespace {

const char* kTickHierarchy =
    "package test;\n"
    "public record BaseTick {\n"
    "    float64 price;\n"
    "    int32 volume;\n"
    "    public float64 notional() { return this.price * (float64) this.volume; }\n"
    "}\n"
    "public record TradeTick extends BaseTick {\n"
    "    float64 commission;\n"
    "}\n";

} // namespace

// 4.1.1 — a derived record is-a base: inherits fields + non-virtual methods
// statically; the layout stays flat (base-field prefix, no vtable/header).
TEST(RecordTests, staticInheritanceFieldsAndMethods) {
    auto src = std::string(kTickHierarchy) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        TradeTick t = TradeTick { price: 2.5, volume: 4, commission: 0.5 };\n"
        "        return (int32)(t.notional() * 10.0 + t.commission * 4.0 + t.price);\n"  // 100+2+2
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 104);

    auto derived = classOf("test.TradeTick");
    ASSERT_NE(derived, nullptr);
    EXPECT_FALSE(derived->hasVtablePointerAtSlotZero());
    auto* st = llvm::dyn_cast<llvm::StructType>(derived->getLlvmType());
    ASSERT_NE(st, nullptr);
    // Flat: { double price, i32 volume, double commission } — base prefix,
    // no vtable slot, no trailing vbase pointer.
    ASSERT_EQ(st->getNumElements(), 3u);
    EXPECT_TRUE(st->getElementType(0)->isDoubleTy());
    EXPECT_TRUE(st->getElementType(1)->isIntegerTy(32));
    EXPECT_TRUE(st->getElementType(2)->isDoubleTy());
}

// 4.1.1 — inherited fields participate in structural equality.
TEST(RecordTests, inheritedFieldsInEquality) {
    auto src = std::string(kTickHierarchy) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        TradeTick a = TradeTick { price: 1.0, volume: 2, commission: 3.0 };\n"
        "        TradeTick b = TradeTick { price: 1.0, volume: 2, commission: 3.0 };\n"
        "        TradeTick c = TradeTick { price: 9.0, volume: 2, commission: 3.0 };\n"
        "        int32 r = 0;\n"
        "        if (a == b) { r = r + 1; }\n"
        "        if (a == c) { r = r + 10; }\n"
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.3 — explicit upcast slices: the base copy holds only base fields;
// the derived original is untouched.
TEST(RecordTests, explicitUpcastSlices) {
    auto src = std::string(kTickHierarchy) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        TradeTick t = TradeTick { price: 2.5, volume: 4, commission: 0.5 };\n"
        "        BaseTick b = (BaseTick) t;\n"
        "        BaseTick expect = BaseTick { price: 2.5, volume: 4 };\n"
        "        int32 r = 0;\n"
        "        if (b == expect) { r = r + 1; }\n"
        "        r = r + (int32)(b.notional() * 10.0 + t.commission * 4.0);\n"  // 100 + 2
        "        return r;\n"  // 103
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 103);
}

// 4.1.2 — overriding/shadowing an inherited method is a compile error.
TEST(RecordCompileErrorTests, inheritedMethodOverrideRejected) {
    auto src = std::string(kTickHierarchy) +
        "public record FeeTick extends BaseTick {\n"
        "    float64 fee;\n"
        "    public float64 notional() { return 0.0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 4.1.3 — implicit upcast is rejected (both declaration and assignment).
TEST(RecordCompileErrorTests, implicitUpcastRejectedAtDecl) {
    auto src = std::string(kTickHierarchy) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        TradeTick t = TradeTick { price: 2.5, volume: 4, commission: 0.5 };\n"
        "        BaseTick b = t;\n"
        "        return (int32) b.price;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

TEST(RecordCompileErrorTests, implicitUpcastRejectedAtAssign) {
    auto src = std::string(kTickHierarchy) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        TradeTick t = TradeTick { price: 2.5, volume: 4, commission: 0.5 };\n"
        "        BaseTick b = BaseTick { price: 0.0, volume: 0 };\n"
        "        b = t;\n"
        "        return (int32) b.price;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 4.1.3 — a downcast/sideways record cast has no meaning and rejects.
TEST(RecordCompileErrorTests, recordDowncastRejected) {
    auto src = std::string(kTickHierarchy) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        BaseTick b = BaseTick { price: 1.0, volume: 2 };\n"
        "        TradeTick t = (TradeTick) b;\n"
        "        return (int32) t.commission;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 4.2.1 — a record extending a reference class is a compile error.
TEST(RecordCompileErrorTests, recordExtendsClassRejected) {
    auto src =
        "package test;\n"
        "public class PlainBase {\n"
        "    public int32 a;\n"
        "}\n"
        "public record RBadExt extends PlainBase {\n"
        "    int32 b;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// ---------------------------------------------------------------------
// Unit 5 — construction extras: positional + defaults (plan §5; spec 4.2–4.4)
// ---------------------------------------------------------------------

// 5.1.1 — un-labeled initializer expressions bind positionally in declared
// order.
TEST(RecordTests, positionalBindingInDeclaredOrder) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { 1.5, 2.25 };\n"
        "        return (int32)(p.x * 10.0 + p.y * 100.0);\n"  // 15 + 225
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 240);
}

// 5.1.1 — positional binding spans inherited fields (ancestors-first, the
// flat layout order).
TEST(RecordTests, positionalBindingWithInheritance) {
    auto src = std::string(kTickHierarchy) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        TradeTick t = TradeTick { 2.5, 4, 0.5 };\n"
        "        return (int32)(t.notional() * 10.0 + t.commission * 4.0 + t.price);\n"  // 104
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 104);
}

// 5.1.1 — arity mismatch is a compile error (too many; too few without
// defaults).
TEST(RecordCompileErrorTests, positionalTooManyRejected) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { 1.0, 2.0, 3.0 };\n"
        "        return (int32) p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

TEST(RecordCompileErrorTests, positionalTooFewRejected) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { 1.0 };\n"
        "        return (int32) p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 5.3.1 — mixing labeled and positional bindings in one initializer rejects.
TEST(RecordCompileErrorTests, mixedLabeledPositionalRejected) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = Point { x: 1.0, 2.0 };\n"
        "        return (int32) p.x;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 5.1.2 — a field with a declared default may be omitted (labeled form);
// the default expression fills it.
TEST(RecordTests, fieldDefaultFillsOmitted) {
    auto src =
        "package test;\n"
        "public record Conf {\n"
        "    float64 rate = 2.5;\n"
        "    int32 retries = 3;\n"
        "    float64 cap;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Conf c = Conf { cap: 10.0 };\n"
        "        return (int32)(c.rate * 4.0 + (float64) c.retries * 100.0 + c.cap);\n"  // 10+300+10
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 320);
}

// 5.1.2 — positional form: trailing omitted fields fill from defaults.
TEST(RecordTests, positionalTrailingDefaultsFill) {
    auto src =
        "package test;\n"
        "public record Conf2 {\n"
        "    float64 a;\n"
        "    float64 b = 9.0;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Conf2 c = Conf2 { 1.0 };\n"
        "        return (int32)(c.a * 10.0 + c.b);\n"  // 10 + 9
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 19);
}

// 5.1.2 — omitting a field WITHOUT a default is a compile error (labeled
// form).
TEST(RecordCompileErrorTests, omittedNonDefaultFieldRejected) {
    auto src =
        "package test;\n"
        "public record Conf3 {\n"
        "    float64 rate = 2.5;\n"
        "    float64 cap;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Conf3 c = Conf3 { rate: 1.0 };\n"
        "        return (int32) c.cap;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// ---------------------------------------------------------------------
// Unit 6 — mutation opt-in: per-field `mut` (plan §6; spec 3.4)
// ---------------------------------------------------------------------

namespace {

const char* kMutTickSrc =
    "package test;\n"
    "public record MTick {\n"
    "    mut float64 price;\n"
    "    int32 volume;\n"
    "    public void reprice(float64 v) { this.price = v; }\n"
    "}\n";

} // namespace

// 6.1.1 — a `mut` field may be written in place (locals and `this` alike);
// the record's other fields keep their values.
TEST(RecordTests, mutFieldWritesInPlace) {
    auto src = std::string(kMutTickSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        MTick t = MTick { price: 1.0, volume: 7 };\n"
        "        t.price = 2.5;\n"
        "        t.reprice(t.price * 2.0);\n"
        "        return (int32)(t.price * 10.0 + (float64) t.volume);\n"  // 50 + 7
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 57);
}

// 6.1.1 — non-`mut` fields still reject writes (default stays immutable).
TEST(RecordCompileErrorTests, nonMutFieldStillRejected) {
    auto src = std::string(kMutTickSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        MTick t = MTick { price: 1.0, volume: 7 };\n"
        "        t.volume = 9;\n"
        "        return t.volume;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.D"));
}

// 6.1.2 — write-through: a `mut` field of a record EMBEDDED in a class
// updates in place through the holder — no new record is constructed (the
// sibling field is untouched and the write is visible through the same
// embedded value).
TEST(RecordTests, mutWriteThroughEmbeddedRecord) {
    auto src = std::string(kMutTickSrc) +
        "public class Book {\n"
        "    MTick top;\n"
        "    public Book() { this.top = MTick { price: 10.0, volume: 3 }; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Book b = heap Book();\n"
        "        b.top.price = 42.0;\n"
        "        return (int32)(b.top.price + (float64)(b.top.volume * 100));\n"  // 42 + 300
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 342);
}

// 6.1.1 corollary — `mut` stays a plain identifier outside the modifier
// position (soft keyword; pre-existing code with `mut` names keeps parsing).
TEST(RecordTests, mutRemainsUsableAsIdentifier) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 mut = 21;\n"
        "        return mut * 2;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// ---------------------------------------------------------------------
// Unit 7 — schema reflectability (plan §7; spec 6.1–6.4)
// ---------------------------------------------------------------------

// 7.1.1 — Class<Point> enumerates field NAMES and TYPE flags in declared
// order (count/offset/size were pinned in Unit 1's reflectionFieldOffsetZero).
TEST(RecordTests, reflectionEnumeratesFieldNamesAndTypes) {
    auto src = std::string(kPointSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Class<Point> c = Point.class;\n"
        "        String f0 #= c.getFieldName(0);\n"
        "        String f1 #= c.getFieldName(1);\n"
        "        int64 t0 = c.getFieldTypeFlags(0);\n"
        "        int32 r = 0;\n"
        "        if (f0 == \"x\") { r = r + 1; }\n"
        "        if (f1 == \"y\") { r = r + 10; }\n"
        "        if ((t0 & 1) != 0) { r = r + 100; }\n"  // primitive bit (float64)
        "        return r;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 111);
}

// 7.1.2 — a record used as a type argument exposes its field set INSIDE the
// instantiation: Box<Tick> reflects over T's fields. DISABLED: `T.class`
// inside a template body does not resolve the substituted type argument —
// ClassLiteralExpression::resolveTypes (Expression.cpp) looks `namedTypeName`
// up in canonicalMap ONLY and never consults the module's active type-
// substitution stack, so `T` stays unresolved and generateCode throws
// CAJETA_ERROR_CLASS_LITERAL. Template-GENERIC gap, not record-specific:
// a plain class type argument fails identically (records ARE registered
// "identically to classes" — 7.2.1 holds). Pending scoping.
TEST(RecordTests, recordTypeArgumentReflectsInsideInstantiation) {
    auto src =
        "package test;\n"
        "public record RfTick {\n"
        "    float64 price;\n"
        "    int32 volume;\n"
        "}\n"
        "public class Box<T> {\n"
        "    public int32 probe() {\n"
        "        Class<T> c = T.class;\n"
        "        int32 r = c.getFieldCount();\n"
        "        String f0 #= c.getFieldName(0);\n"
        "        if (f0 == \"price\") { r = r + 100; }\n"
        "        return r;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<RfTick> b = heap Box<RfTick>();\n"
        "        return b.probe();\n"  // 2 + 100
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 102);
}
