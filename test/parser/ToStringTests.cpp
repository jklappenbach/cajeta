// Tests for the @ToString Lombok-mirror synthesizer
// (docs/specification/reflect/Annotations.md § @ToString). v1 ships two
// formats: PROPERTIES (default) — `ClassName(field1=v1,field2=v2,...)`
// — and JSON — `{"field1":v1,"field2":v2,...}` (String fields get
// JSON-quoted + escaped via `__cajeta_json_quote_str`).
//
// Each test compiles a Cajeta source and calls a `static char* run()`
// that invokes toString() on a heap instance and returns the result.
// The C++ side compares against expected strings.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <cstring>
#include <string>

using cajeta_test::CajetaJit;

namespace {
    // After Phase 2b-β, `String` returns are pointers to a
    // `cajeta.lang.String` struct: { vtable, bytes_ptr,
    // byteLength, mode, cachedCpLength }. Bytes_ptr points to a
    // CajetaArray-shaped struct `{ i64 count, [N x i8] data }`. Pull
    // the byte payload out and copy it into a std::string sized by
    // byteLength (no null-terminator dependence). For the bootstrap
    // window where the synthesizer returns a raw malloc'd C-string
    // — same flow but the runtime fields land at different offsets
    // — the heuristic is to detect a printable ASCII byte at offset 0
    // and fall back to strlen.
    struct CajetaStringLayout {
        const void* vtable;
        const void* bytes;
        int32_t byteLength;
        int32_t mode;
        int32_t cachedCpLength;
    };

    std::string runToString(const std::string& src) {
        try {
            auto jit = CajetaJit::compile(src, "test.D");
            auto fn = jit->lookup<const CajetaStringLayout* (*)()>("run");
            const CajetaStringLayout* s = fn();
            if (!s) return std::string("<null>");
            // Two return shapes share this path: a real cajeta.lang.String
            // struct (literal returns) and a legacy malloc'd C-string
            // (__cajeta_str_concat, used by the synthesizer). Discriminate
            // deterministically by comparing the leading field against the
            // real String vtable address — NOT by sniffing pointer bytes.
            // The old byte heuristic misfired whenever the JIT happened to
            // place the String vtable global at an address whose low two
            // bytes were both printable ASCII (~14% chance), which is why it
            // was MachO/COFF-flaky while passing on ELF by luck of layout.
            const void* strVtable = jit->lookupRawSymbol("cajeta.lang.String#VTable");
            if (strVtable && s->vtable == strVtable) {
                // Real cajeta.lang.String — read the bytes field.
                if (!s->bytes || s->byteLength <= 0) return std::string("");
                // bytes points to { i64 count, [N x i8] data }. Skip past
                // the 8-byte count header to get the data.
                const char* data = (const char*) s->bytes + sizeof(int64_t);
                return std::string(data, (size_t) s->byteLength);
            }
            // Legacy malloc'd char* path — read as C-string.
            return std::string((const char*) s);
        } catch (cajeta::Exception& e) {
            std::cerr << "[CAUGHT cajeta::Exception]: " << e.getMessage()
                << " errorId=" << e.getErrorId() << std::endl;
            throw;
        }
    }
}

// Single int field — the simplest possible case.
TEST(ToStringTests, singleIntField) {
    auto src =
        "package test;\n"
        "@ToString public class P {\n"
        "    public int32 n;\n"
        "    public P(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        P p = heap P(42);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "P(n=42)");
}

// Multiple primitive fields — exercises the comma separator + multi-width handling.
TEST(ToStringTests, multipleIntFields) {
    auto src =
        "package test;\n"
        "@ToString public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public Point(int32 a, int32 b) { this.x = a; this.y = b; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Point p = heap Point(3, 7);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Point(x=3,y=7)");
}

// Mixed primitive widths.
TEST(ToStringTests, mixedPrimitiveWidths) {
    auto src =
        "package test;\n"
        "@ToString public class M {\n"
        "    public int8  a;\n"
        "    public int16 b;\n"
        "    public int32 c;\n"
        "    public int64 d;\n"
        "    public M(int8 a, int16 b, int32 c, int64 d) {\n"
        "        this.a = a; this.b = b; this.c = c; this.d = d;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        M m = heap M(1, 2, 3, 4);\n"
        "        return m.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "M(a=1,b=2,c=3,d=4)");
}

// Boolean field.
TEST(ToStringTests, booleanField) {
    auto src =
        "package test;\n"
        "@ToString public class Flag {\n"
        "    public boolean on;\n"
        "    public Flag(boolean v) { this.on = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Flag f = heap Flag(true);\n"
        "        return f.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Flag(on=true)");
}

// Float field — uses %g formatting.
TEST(ToStringTests, floatField) {
    auto src =
        "package test;\n"
        "@ToString public class Measure {\n"
        "    public float64 ratio;\n"
        "    public Measure(float64 r) { this.ratio = r; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Measure m = heap Measure(0.5);\n"
        "        return m.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Measure(ratio=0.5)");
}

// @ToString.Exclude omits the field.
TEST(ToStringTests, excludeFieldOmitted) {
    auto src =
        "package test;\n"
        "@ToString public class Secret {\n"
        "    public int32 visible;\n"
        "    @Exclude public int32 hidden;\n"
        "    public Secret(int32 v, int32 h) { this.visible = v; this.hidden = h; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Secret s = heap Secret(1, 99);\n"
        "        return s.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Secret(visible=1)");
}

// Static fields skipped automatically.
TEST(ToStringTests, staticFieldsSkipped) {
    auto src =
        "package test;\n"
        "@ToString public class Counter {\n"
        "    public static int32 total = 999;\n"
        "    public int32 n;\n"
        "    public Counter(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Counter c = heap Counter(5);\n"
        "        return c.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Counter(n=5)");
}

// Empty class (no instance fields) still produces a valid toString.
TEST(ToStringTests, emptyClass) {
    auto src =
        "package test;\n"
        "@ToString public class Empty {\n"
        "    public Empty() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Empty e = heap Empty();\n"
        "        return e.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Empty()");
}

// User-declared toString() wins — synthesizer skips.
TEST(ToStringTests, userToStringWins) {
    auto src =
        "package test;\n"
        "@ToString public class Custom {\n"
        "    public int32 n;\n"
        "    public Custom(int32 v) { this.n = v; }\n"
        "    public String toString() { return \"custom\"; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Custom c = heap Custom(7);\n"
        "        return c.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "custom");
}

// Negative ints rendered correctly.
TEST(ToStringTests, negativeInt) {
    auto src =
        "package test;\n"
        "@ToString public class Neg {\n"
        "    public int32 n;\n"
        "    public Neg(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Neg n = heap Neg(-7);\n"
        "        return n.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Neg(n=-7)");
}

// TO_STRING_JSON renders single-field with int primitive.
TEST(ToStringTests, jsonSingleIntField) {
    auto src =
        "package test;\n"
        "@ToString(format=\"TO_STRING_JSON\") public class P {\n"
        "    public int32 n;\n"
        "    public P(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        P p = heap P(42);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "{\"n\":42}");
}

// TO_STRING_JSON renders multiple fields with comma separators.
TEST(ToStringTests, jsonMultipleFields) {
    auto src =
        "package test;\n"
        "@ToString(format=\"TO_STRING_JSON\") public class Pt {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public boolean live;\n"
        "    public Pt(int32 a, int32 b, boolean l) {\n"
        "        this.x = a; this.y = b; this.live = l;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Pt p = heap Pt(3, 7, true);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "{\"x\":3,\"y\":7,\"live\":true}");
}

// TO_STRING_JSON quotes and escapes String fields.
TEST(ToStringTests, jsonStringFieldQuoted) {
    auto src =
        "package test;\n"
        "@ToString(format=\"TO_STRING_JSON\") public class S {\n"
        "    public String name;\n"
        "    public S(String s) { this.name = s; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        S s = heap S(\"hello\");\n"
        "        return s.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "{\"name\":\"hello\"}");
}

// TO_STRING_JSON escapes embedded `"` and `\` per RFC 8259.
TEST(ToStringTests, jsonStringFieldEscapes) {
    auto src =
        "package test;\n"
        "@ToString(format=\"TO_STRING_JSON\") public class S {\n"
        "    public String name;\n"
        "    public S(String s) { this.name = s; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        S s = heap S(\"a\\\"b\\\\c\");\n"
        "        return s.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "{\"name\":\"a\\\"b\\\\c\"}");
}

// TO_STRING_JSON on an empty class is the empty JSON object.
TEST(ToStringTests, jsonEmptyClass) {
    auto src =
        "package test;\n"
        "@ToString(format=\"TO_STRING_JSON\") public class E {\n"
        "    public E() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        E e = heap E();\n"
        "        return e.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "{}");
}

// Unknown format string still rejected (TO_STRING_BAD_FORMAT).
TEST(ToStringTests, unknownFormatRejected) {
    auto src =
        "package test;\n"
        "@ToString(format=\"TO_STRING_NONSENSE\") public class P {\n"
        "    public int32 n;\n"
        "    public P() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_TOSTRING_BAD_FORMAT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TOSTRING_BAD_FORMAT");
    }
}

// Sanity: no @ToString → no synthesis (so calling toString() goes to
// whatever Object provides, currently a null stub — just verify the
// class compiles).
TEST(ToStringTests, noAnnotationCompilesNormally) {
    auto src =
        "package test;\n"
        "public class Plain {\n"
        "    public int32 n;\n"
        "    public Plain(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Plain p = heap Plain(5);\n"
        "        return p.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

// `@ToString(of={"a","b"})` — allowlist of fields. Only listed fields
// are rendered, in the listed order (independent of declaration order).
TEST(ToStringTests, ofAllowlistOrdersByListedFields) {
    auto src =
        "package test;\n"
        "@ToString(of={\"b\",\"a\"}) public class P {\n"
        "    public int32 a;\n"
        "    public int32 b;\n"
        "    public int32 c;\n"
        "    public P(int32 av, int32 bv, int32 cv) {\n"
        "        this.a = av; this.b = bv; this.c = cv;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        P p = heap P(1, 2, 3);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "P(b=2,a=1)");
}

// `@ToString(of={...})` empty allowlist renders no fields.
TEST(ToStringTests, ofEmptyAllowlistOmitsAllFields) {
    auto src =
        "package test;\n"
        "@ToString(of={}) public class P {\n"
        "    public int32 a;\n"
        "    public P() { this.a = 7; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        P p = heap P();\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "P()");
}

// `@ToString(callSuper=true)` includes `super=Parent(...)` as the first
// rendered field, with the parent's toString output verbatim (parent
// should also have @ToString or a hand-written toString for this to
// produce a useful result).
//
// Note: this test uses a literal `super(N)` argument because passing a
// ctor parameter via `super(param)` hits a pre-existing IR-emission
// bug (signature mismatch — observed under SuperProbe, deferred).
// The shape under test here is the toString synth's callSuper path,
// which is orthogonal.
TEST(ToStringTests, callSuperPrependsParentToString) {
    auto src =
        "package test;\n"
        "@ToString public class Base {\n"
        "    public int32 b;\n"
        "    public Base() { this.b = 3; }\n"
        "}\n"
        "@ToString(callSuper=true) public class Child extends Base {\n"
        "    public int32 c;\n"
        "    public Child(int32 cv) { this.c = cv; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        Child x = heap Child(7);\n"
        "        return x.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "Child(super=Base(b=3),c=7)");
}

// JSON format honors `of` too.
TEST(ToStringTests, jsonOfAllowlist) {
    auto src =
        "package test;\n"
        "@ToString(format=\"TO_STRING_JSON\", of={\"a\"}) public class P {\n"
        "    public int32 a;\n"
        "    public int32 b;\n"
        "    public P(int32 av, int32 bv) { this.a = av; this.b = bv; }\n"
        "}\n"
        "public final class D {\n"
        "    public static String run() {\n"
        "        P p = heap P(11, 22);\n"
        "        return p.toString();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runToString(src), "{\"a\":11}");
}

// `of` referencing an unknown field is rejected at synthesis time.
TEST(ToStringTests, ofUnknownFieldRejected) {
    auto src =
        "package test;\n"
        "@ToString(of={\"nope\"}) public class P {\n"
        "    public int32 a;\n"
        "    public P() { this.a = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_TOSTRING_UNKNOWN_FIELD");
    }
}
