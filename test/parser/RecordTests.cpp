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

// 1.1.4 / 1.3.1 — no per-instance header: the record's LLVM struct is just
// the field slots (no vtable pointer at slot 0), so offset-0 is the first
// field and the value size is the sum of the field sizes.

// 1.1.4 — cajeta-side reflection agrees: first field at offset 0, two fields,
// instance size == sum of field sizes (16). The class literal is the record's
// reflection entry point — `Class.of(instance)` reads a header records don't
// have.

// 1.3.1 — the record VALUE in emitted IR is the bare field-bytes struct: no
// vtable/header slot. (A module-level `#VTable`/RTTI global still exists —
// that is the Class<Point> reflection surface, not per-instance state.)

// 1.1.5 — a template record monomorphizes per type-arg list; every
// instantiation is a value type.

// 1.1.6 — a record field whose type is another record embeds as a value
// sub-aggregate (inline struct, not a pointer).

// ---------------------------------------------------------------------
// Unit 2 — constraint gates (plan §2; spec 1.3, 2.4, 2.5.4, 2.6.4–2.6.5)
// ---------------------------------------------------------------------

// 2.1.1 — a record may carry pure methods + operator overloads; both dispatch
// directly (no per-instance vtable exists to dispatch through).
// 2.2.3 — a small record operator is force-inlined (@ValueType alwaysinline
// path): no CALL to it survives in the O0 post-AlwaysInline IR.

// 2.1.3 — a @ValueType class field is allowed in a record (value aggregates
// compose); one more leaf of the value-only field rule.

// 2.1.4 — one-way containment: a (reference) class may hold a record field.

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

// ---------------------------------------------------------------------
// Unit 3 — immutability + copy-with (plan §3; spec 3.2, 3.3)
// ---------------------------------------------------------------------

// 3.1.1 — a field write on a (default-immutable) record is a compile error.

// 3.1.1 — a record's own non-constructor method may not mutate `this` either.

// 3.1.1 corollary — a record CONSTRUCTOR still initializes its own fields.

// 3.1.1 corollary — a CLASS field that holds a record stays assignable
// (the receiver is the class; the record value is replaced wholesale).

// 3.1.2 — with(field: v) yields a NEW record: one field replaced, the rest
// copied, the original untouched.

// 3.3.1 — with round-trips field values exactly: replacing a field with the
// original value compares structurally equal; multi-field with works.

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

// 4.1.1 — inherited fields participate in structural equality.

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
        "        String f0 = c.getFieldName(0);\n"
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
