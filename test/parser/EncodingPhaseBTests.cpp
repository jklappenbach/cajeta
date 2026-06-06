// Phase B tests for @Encoding(EncoderClass) — the actual synthesizer.
//
// The synthesizer emits:
//   - `T(byte[] bytes)` ctor that calls EncoderClass.decode(bytes)
//     and memcpys the decoded body into `this`.
//   - `#byte[] toBytes()` that calls EncoderClass.encode(this).
//
// Tests use simple identity-style encoders. Real encoder
// implementations (JSON / MessagePack / Protobuf) ship later under
// `cajeta.codec` once that subsystem lands (Features.md S-1101 / S-1102).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Basic dispatch: heap T(bytes) calls decode, T.toBytes() calls encode.
TEST(EncodingPhaseBTests, dispatchCallsEncoderMethods) {
    auto src =
        "package test;\n"
        "public class Enc {\n"
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
        "        int8[] bytes = heap int8[1];\n"
        "        bytes[0] = 7;\n"
        "        M m = heap M(bytes);\n"
        "        return m.id;\n"  // decode set 7
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// toBytes() returns whatever encode produces.
TEST(EncodingPhaseBTests, toBytesCallsEncode) {
    auto src =
        "package test;\n"
        "public class Enc {\n"
        "    public static #int8[] encode(M v) {\n"
        "        int8[] out = heap int8[1];\n"
        "        out[0] = (int8) v.id;\n"  // simple copy
        "        return out;\n"
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
        "    public static int32 run() {\n"
        "        M m = heap M();\n"
        "        m.id = 17;\n"
        "        int8[] bytes = m.toBytes();\n"
        "        return (int32) bytes[0];\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 17);
}

// User-declared T(byte[]) wins — synthesizer skips.
TEST(EncodingPhaseBTests, userCtorWinsOverSynthesized) {
    auto src =
        "package test;\n"
        "public class Enc {\n"
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
        "    public M(int8[] bytes) { this.id = 999; }\n"  // user-declared wins
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] bytes = heap int8[1];\n"
        "        bytes[0] = 7;\n"
        "        M m = heap M(bytes);\n"
        "        return m.id;\n"  // user-declared sets 999, not 7
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 999);
}

// Missing static `encode` method — clear error from the synth.
TEST(EncodingPhaseBTests, encoderMissingEncodeRejected) {
    auto src =
        "package test;\n"
        "public class Enc {\n"
        // Has decode but no encode.
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
        FAIL() << "expected CAJETA_ERROR_ENCODING_ENCODE_MISSING";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ENCODING_ENCODE_MISSING");
    }
}

// Encoder class doesn't exist — clear error.
TEST(EncodingPhaseBTests, encoderClassNotFoundRejected) {
    auto src =
        "package test;\n"
        "@Encoding(NonExistent.class)\n"
        "public class M {\n"
        "    public int32 id;\n"
        "    public M() { this.id = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_ENCODING_ENCODER_NOT_FOUND";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_ENCODING_ENCODER_NOT_FOUND");
    }
}
