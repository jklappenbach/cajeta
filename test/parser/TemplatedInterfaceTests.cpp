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
        "        int8[] out = heap int8[1];\n"
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
        "        int8[] bytes #= m.toBytes();\n"
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
        "        return heap int8[1];\n"
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
        FAIL() << "expected rejection of mismatched encoder";
    } catch (cajeta::Exception& e) {
        // Either error is a valid rejection. INTERFACE_NOT_IMPLEMENTED
        // is what fires once Encoder<N>'s vtable is real and the
        // signature check sees `encode(M v)` doesn't satisfy
        // `encode(N v)`. ENCODER_T_MISMATCH was the pre-fix error
        // from @Encoding's verification path, which used to be the
        // only signal when templated-interface vtables were
        // short-circuited.
        const std::string& id = e.getErrorId();
        EXPECT_TRUE(id == "CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED"
                    || id == "CAJETA_ERROR_ENCODING_ENCODER_T_MISMATCH")
            << "unexpected error id: " << id;
    }
}

// Primitive type arg through a templated interface dispatch.
// `implements Codec<int32>` and `Codec<int32> c = heap IntCodec()`
// — the int32 type arg must thread through the implements-clause
// visitor and the resolveImplementedInterfaces -> instantiate path.
TEST(TemplatedInterfaceTests, dispatchThroughTemplatedInterfacePrimitiveArg) {
    auto src =
        "package test;\n"
        "public interface Codec<T> {\n"
        "    int32 encode(T v);\n"
        "}\n"
        "public class IntCodec implements Codec<int32> {\n"
        "    public IntCodec() { }\n"
        "    public int32 encode(int32 v) { return v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Codec<int32> c = heap IntCodec();\n"
        "        return c.encode(5);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 6);
}

// Diagnostic: assignment to a templated-interface local without
// dispatching through it. If THIS crashes, the fat-pointer assembly
// is the problem; if it passes, dispatch is.
TEST(TemplatedInterfaceTests, templatedInterfaceLocalAssignmentOnly) {
    auto src =
        "package test;\n"
        "public interface Codec<T> {\n"
        "    int32 encode(T v);\n"
        "}\n"
        "public class IntCodec implements Codec<int32> {\n"
        "    public IntCodec() { }\n"
        "    public int32 encode(int32 v) { return v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Codec<int32> c = heap IntCodec();\n"
        "        return 0;\n"  // do NOT dispatch; just compile-and-assign.
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// Diagnostic: just compile + direct-call (no interface dispatch).
// If this passes but dispatchThroughTemplatedInterface crashes, the
// problem is at the interface-fat-pointer / dispatch step.
TEST(TemplatedInterfaceTests, implementsTemplatedInterfaceDirectCall) {
    auto src =
        "package test;\n"
        "public interface Codec<T> {\n"
        "    int32 encode(T v);\n"
        "}\n"
        "public class IntCodec implements Codec<int32> {\n"
        "    public IntCodec() { }\n"
        "    public int32 encode(int32 v) { return v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IntCodec ic = heap IntCodec();\n"
        "        return ic.encode(5);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 6);
}

// A class implements a user-defined templated interface and an
// interface-typed reference dispatches into the impl. This exercises
// the per-(impl, interface<T>) vtable path that was short-circuited
// previously (the instantiator would return the template itself,
// leaving an interface-typed reference with no real vtable to dispatch
// through). T is a class type because the implements-clause visitor
// captures only class-typed args today; primitive args land in v2.
TEST(TemplatedInterfaceTests, dispatchThroughTemplatedInterface) {
    auto src =
        "package test;\n"
        "public class Payload {\n"
        "    public int32 value;\n"
        "    public Payload(int32 v) { this.value = v; }\n"
        "}\n"
        "public interface Codec<T> {\n"
        "    int32 encode(T v);\n"
        "}\n"
        "public class PayloadCodec implements Codec<Payload> {\n"
        "    public PayloadCodec() { }\n"
        "    public int32 encode(Payload v) { return v.value + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Codec<Payload> c = heap PayloadCodec();\n"
        "        Payload p = heap Payload(5);\n"
        "        return c.encode(p);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 6);
}

// `implements Encoder` without a type argument is a compile error —
// the contract requires the parameterized form.
TEST(TemplatedInterfaceTests, encodingRejectsBareImplementsEncoder) {
    auto src =
        "package test;\n"
        "import cajeta.wire.Encoder;\n"
        "public class Enc implements Encoder {\n"
        "    public static #int8[] encode(M v) {\n"
        "        return heap int8[1];\n"
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
