// Tests for the constructor Lombok-mirror synthesizers
// (docs/specification/reflect/Annotations.md § Constructors):
//   - @NoArgsConstructor
//   - @AllArgsConstructor
//   - @RequiredArgsConstructor (currently selects `final` fields only;
//     @NonNull selection lands when @NonNull does)
//
// Each synthesizer skips silently when a same-arity ctor already
// exists on the class.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// @NoArgsConstructor on a class with no user-declared ctor → that
// ctor exists and zero-inits every field.
TEST(ConstructorAnnotationTests, noArgsConstructorZeroInits) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor public class P {\n"
        "    public int32 a;\n"
        "    public int32 b;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P();\n"
        "        return p.a + p.b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// @AllArgsConstructor takes every field in declaration order.
TEST(ConstructorAnnotationTests, allArgsConstructor) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor public class Point {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Point p = heap Point(3, 4);\n"
        "        return p.x + p.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// @AllArgsConstructor with mixed primitive widths.
TEST(ConstructorAnnotationTests, allArgsConstructorMixedWidths) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor public class M {\n"
        "    public int8  a;\n"
        "    public int16 b;\n"
        "    public int32 c;\n"
        "    public int64 d;\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        M m = heap M(1, 2, 3, 4);\n"
        "        return ((int64) m.a) + ((int64) m.b)\n"
        "             + ((int64) m.c) + m.d;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 10);
}

// @RequiredArgsConstructor picks only `final` fields.
TEST(ConstructorAnnotationTests, requiredArgsCtorPicksFinalFields) {
    auto src =
        "package test;\n"
        "@RequiredArgsConstructor public class C {\n"
        "    public final int32 fixed;\n"
        "    public int32 mutable_;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        C c = heap C(42);\n"  // single-arg matches the only `final` field
        "        return c.fixed + c.mutable_;\n"  // mutable_ stays 0
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// @RequiredArgsConstructor with no qualifying fields → zero-arg ctor.
TEST(ConstructorAnnotationTests, requiredArgsCtorNoFinalFields) {
    auto src =
        "package test;\n"
        "@RequiredArgsConstructor public class E {\n"
        "    public int32 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        E e = heap E();\n"
        "        return e.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// User-declared ctor with matching arity wins — synthesizer skips.
TEST(ConstructorAnnotationTests, userCtorWinsOverSynthesized) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor public class P {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "    public P(int32 a, int32 b) { this.x = a + 100; this.y = b + 100; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(3, 4);\n"
        "        return p.x + p.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 207);  // (103 + 104)
}

// @NoArgsConstructor + @AllArgsConstructor on the same class produces
// BOTH ctors.
TEST(ConstructorAnnotationTests, noArgsAndAllArgsCoexist) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor @AllArgsConstructor public class P {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P a = heap P();\n"
        "        P b = heap P(7, 8);\n"
        "        return (a.x + a.y) * 100 + (b.x + b.y);\n"  // 0*100 + 15 = 15
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 15);
}

// Sanity: no annotation → no synthesized ctor (default behavior unchanged).
TEST(ConstructorAnnotationTests, noAnnotationKeepsDefaultBehavior) {
    auto src =
        "package test;\n"
        "public class P {\n"
        "    public int32 n;\n"
        "    public P(int32 v) { this.n = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(7);\n"
        "        return p.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// @NoArgsConstructor(access="private") — modifier lands on the synth'd ctor
// and compilation succeeds (visibility enforcement at construction sites
// is a future ticket — see Annotations.md § Accessors).
TEST(ConstructorAnnotationTests, noArgsConstructorAccessPrivate) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor(access=\"private\") public class P {\n"
        "    public int32 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P();\n"
        "        return p.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// @AllArgsConstructor(access="protected") same story.
TEST(ConstructorAnnotationTests, allArgsConstructorAccessProtected) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor(access=\"protected\") public class P {\n"
        "    public int32 a;\n"
        "    public int32 b;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(3, 4);\n"
        "        return p.a + p.b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// @RequiredArgsConstructor(access="public") explicit — works like default.
TEST(ConstructorAnnotationTests, requiredArgsConstructorAccessPublicExplicit) {
    auto src =
        "package test;\n"
        "@RequiredArgsConstructor(access=\"public\") public class P {\n"
        "    public final int32 n;\n"
        "    public int32 ignored;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(42);\n"
        "        return p.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Unknown access value rejected with the same error ID the @Getter/@Setter
// path uses.
TEST(ConstructorAnnotationTests, accessUnknownRejected) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor(access=\"bogus\") public class P {\n"
        "    public int32 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ACCESSOR_BAD_ACCESS";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ACCESSOR_BAD_ACCESS");
    }
}

// @AllArgsConstructor(staticName="of") synthesizes a public static
// factory `T.of(args...)` that calls the (now-private) ctor and
// returns the heap instance. Mirrors Lombok's pattern.
TEST(ConstructorAnnotationTests, allArgsStaticNameOf) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor(staticName=\"of\") public class Pt {\n"
        "    public int32 x;\n"
        "    public int32 y;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pt p = Pt.of(3, 4);\n"
        "        return p.x * 10 + p.y;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 34);
}

// @NoArgsConstructor(staticName="empty") works the same way for zero
// args — `T.empty()` returns a zero-initialized instance.
TEST(ConstructorAnnotationTests, noArgsStaticNameEmpty) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor(staticName=\"empty\") public class P {\n"
        "    public int32 n;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = P.empty();\n"
        "        return p.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// @RequiredArgsConstructor(staticName="create") picks only the final
// fields and exposes them through the static factory.
TEST(ConstructorAnnotationTests, requiredArgsStaticNameCreate) {
    auto src =
        "package test;\n"
        "@RequiredArgsConstructor(staticName=\"create\") public class P {\n"
        "    public final int32 id;\n"
        "    public int32 extra;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = P.create(99);\n"
        "        return p.id;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// `access` arg applies to the FACTORY when staticName is set; the
// ctor is force-marked PRIVATE (Lombok parity). Visibility enforcement
// at call sites is a separate ticket — the modifier lands on both
// methods correctly here, and compilation succeeds.
TEST(ConstructorAnnotationTests, staticNameAccessAppliesToFactory) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor(access=\"protected\", staticName=\"of\")\n"
        "public class Pt {\n"
        "    public int32 v;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Pt p = Pt.of(42);\n"
        "        return p.v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

// Instance-field initializers fire in the synthesized no-args ctor.
TEST(ConstructorAnnotationTests, instanceFieldInitNoArgsCtor) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor public class P {\n"
        "    public int32 a = 42;\n"
        "    public int32 b = 7;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P();\n"
        "        return p.a * 100 + p.b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 4207);
}

// @AllArgsConstructor: ctor args override initializers.
TEST(ConstructorAnnotationTests, instanceFieldInitAllArgsOverridden) {
    auto src =
        "package test;\n"
        "@AllArgsConstructor public class P {\n"
        "    public int32 a = 42;\n"
        "    public int32 b = 7;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(1, 2);\n"
        "        return p.a * 100 + p.b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 102);
}

// @RequiredArgsConstructor: non-required fields with initializers
// pick up their defaults; required fields get the ctor arg.
TEST(ConstructorAnnotationTests, instanceFieldInitRequiredArgs) {
    auto src =
        "package test;\n"
        "@RequiredArgsConstructor public class P {\n"
        "    public final int32 id;\n"
        "    public int32 extra = 99;\n"  // initializer, not required
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(5);\n"
        "        return p.id * 100 + p.extra;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 599);
}

// A user-written ctor body picks up field initializers BEFORE the
// body executes — Java semantics: field initializers run as part of
// the implicit-construction sequence (after super()/this() resolves),
// then the ctor body. Regression test for the gap where only
// synthesized ctors picked up initializers.
TEST(ConstructorAnnotationTests, instanceFieldInitUserCtorNoArgs) {
    auto src =
        "package test;\n"
        "public class P {\n"
        "    public int32 a = 42;\n"
        "    public int32 b = 7;\n"
        "    public P() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P();\n"
        "        return p.a * 100 + p.b;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 4207);
}

// A user-written ctor that ALSO writes the field overrides the
// initializer (Java semantics: initializer runs first, then body).
TEST(ConstructorAnnotationTests, instanceFieldInitUserCtorOverwrites) {
    auto src =
        "package test;\n"
        "public class P {\n"
        "    public int32 a = 42;\n"
        "    public int32 b = 7;\n"
        "    public P(int32 newA) { this.a = newA; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        P p = heap P(99);\n"
        "        return p.a * 100 + p.b;\n"  // 99*100 + 7 (b untouched, initialized)
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 9907);
}

// User ctor with mixed-type initializers (float, bool).
TEST(ConstructorAnnotationTests, instanceFieldInitUserCtorMixedTypes) {
    auto src =
        "package test;\n"
        "public class P {\n"
        "    public boolean ready = true;\n"
        "    public float64 ratio = 0.5;\n"
        "    public int64 count = 1000;\n"
        "    public P() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        P p = heap P();\n"
        "        if (!p.ready) { return -1; }\n"
        "        return p.count + (int64) (p.ratio * 100.0);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 1050);
}

// Float and boolean initializers.
TEST(ConstructorAnnotationTests, instanceFieldInitMixedTypes) {
    auto src =
        "package test;\n"
        "@NoArgsConstructor public class P {\n"
        "    public boolean ready = true;\n"
        "    public float64 ratio = 0.25;\n"
        "    public int64 count = 1000;\n"
        "}\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        P p = heap P();\n"
        "        if (!p.ready) { return -1; }\n"
        "        return p.count + (int64) (p.ratio * 100.0);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 1025);
}
