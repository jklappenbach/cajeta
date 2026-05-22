// Tests that templated interfaces parse and can be referenced.
// Pre-fix, adding `interface Foo<T> { ... }` to stdlib crashed the
// stdlib build at parse time because visitInterfaceDeclaration
// didn't process typeParameters — the body methods referenced
// unresolved T placeholders.
//
// v1: declaration parses; class can declare `implements Foo<X>` and
// the type arguments are captured at parse time on the implementer
// (CajetaClass::qImplementedTypeArgs). The @Encoding path uses
// those args to verify the encoder's Encoder<T> conformance when
// the encoder opted in via `implements Encoder<T>`. Encoders without
// the implements clause still work via the duck-typed dispatch path
// — the verification is hard on declared-mismatch, soft on absence.
// Full per-(impl, interface<T>) vtable instantiation is future work.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// User-defined templated interface compiles.
TEST(TemplatedInterfaceTests, declarationCompiles) {
    auto src =
        "package test;\n"
        "public interface Codec<T> {\n"
        "    int32 encode(T v);\n"
        "    T decode(int32 raw);\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Stdlib's cajeta.wire.Encoder<T> is reachable from user code.
TEST(TemplatedInterfaceTests, stdlibEncoderReachable) {
    auto src =
        "package test;\n"
        "import cajeta.wire.Encoder;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // Just reference the type — no instantiation needed.
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// @Encoding accepts an encoder that declares `implements Encoder<T>`
// with the correct T. The verification path runs; dispatch is the
// same duck-typed mechanism.
TEST(TemplatedInterfaceTests, encodingAcceptsMatchingImplements) {
    auto src =
        "package test;\n"
        "import cajeta.wire.Encoder;\n"
        "public class Enc implements Encoder<M> {\n"
        "    public static #int8[] encode(M v) {\n"
        "        int8[] out = new int8[1];\n"
        "        out[0] = (int8) v.id;\n"
        "        return out;\n"
        "    }\n"
        "    public static #M decode(int8[] b) {\n"
        "        M m = heap M();\n"
        "        m.id = (int32) b[0];\n"
        "        return m;\n"
        "    }\n"
        "}\n"
        "@Encoding(Enc.class)\n"
        "public class M {\n"
        "    public int32 id;\n"
        "    public M() { this.id = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        M m = heap M();\n"
        "        m.id = 5;\n"
        "        int8[] bytes = m.toBytes();\n"
        "        return (int32) bytes[0];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

// @Encoding rejects an encoder that declares `implements Encoder<U>`
// where U is the wrong type. The verification path catches the
// mismatch at compile time with a clear error.
TEST(TemplatedInterfaceTests, encodingRejectsWrongImplementsTypeArg) {
    auto src =
        "package test;\n"
        "import cajeta.wire.Encoder;\n"
        "public class N {\n"
        "    public int32 value;\n"
        "    public N() { this.value = 0; }\n"
        "}\n"
        "public class Enc implements Encoder<N> {\n"
        "    public static #int8[] encode(M v) {\n"
        "        return new int8[1];\n"
        "    }\n"
        "    public static #M decode(int8[] b) {\n"
        "        return heap M();\n"
        "    }\n"
        "}\n"
        "@Encoding(Enc.class)\n"
        "public class M {\n"
        "    public int32 id;\n"
        "    public M() { this.id = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ENCODING_ENCODER_T_MISMATCH";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ENCODING_ENCODER_T_MISMATCH");
    }
}

// `implements Encoder` without a type argument is a compile error —
// the contract requires the parameterized form.
TEST(TemplatedInterfaceTests, encodingRejectsBareImplementsEncoder) {
    auto src =
        "package test;\n"
        "import cajeta.wire.Encoder;\n"
        "public class Enc implements Encoder {\n"
        "    public static #int8[] encode(M v) {\n"
        "        return new int8[1];\n"
        "    }\n"
        "    public static #M decode(int8[] b) {\n"
        "        return heap M();\n"
        "    }\n"
        "}\n"
        "@Encoding(Enc.class)\n"
        "public class M {\n"
        "    public int32 id;\n"
        "    public M() { this.id = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ENCODING_ENCODER_BAD_ARITY";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ENCODING_ENCODER_BAD_ARITY");
    }
}
