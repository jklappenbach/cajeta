// MD5 tests. Pin against RFC 1321 / Wikipedia test vectors.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <cstdio>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string runHashHex(const std::string& input) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.MD5;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        "        int8[] data = heap int8[" + std::to_string(input.size()) + "];\n";
    for (size_t i = 0; i < input.size(); i++) {
        src += "        data[" + std::to_string(i) + "L] = (int8) "
             + std::to_string((int)(signed char) input[i]) + ";\n";
    }
    src += "        String s = MD5.hashHex(data, " + std::to_string(input.size()) + "L);\n"
        "        int8[] out = s.toBytes();\n"
        "        return #out;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    if (!hdr) return "<null>";
    int64_t count = *(int64_t*) hdr;
    const char* data = (const char*) hdr + 8;
    return std::string(data, (size_t) count);
}

// Hex digest of `count` copies of byte `val`, filled with a loop (not per-byte
// unrolling) so large inputs stay cheap to JIT. Exercises the bulk-update fast
// path (len >> 64), where whole blocks are hashed straight from the caller
// buffer with no per-block memcpy.
std::string runHashHexRepeated(int val, size_t count) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.MD5;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        "        int64 n = " + std::to_string(count) + "L;\n"
        "        int8[] data = heap int8[n];\n"
        "        int64 i = 0L;\n"
        "        while (i < n) { data[i] = (int8) " + std::to_string(val) + "; i = i + 1L; }\n"
        "        String s = MD5.hashHex(data, n);\n"
        "        int8[] out = s.toBytes();\n"
        "        return #out;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    if (!hdr) return "<null>";
    int64_t count2 = *(int64_t*) hdr;
    const char* data = (const char*) hdr + 8;
    return std::string(data, (size_t) count2);
}

} // namespace

TEST(MD5Tests, emptyString) {
    EXPECT_EQ(runHashHex(""), "d41d8cd98f00b204e9800998ecf8427e");
}

TEST(MD5Tests, singleCharA) {
    EXPECT_EQ(runHashHex("a"), "0cc175b9c0f1b6a831c399e269772661");
}

TEST(MD5Tests, abc) {
    EXPECT_EQ(runHashHex("abc"), "900150983cd24fb0d6963f7d28e17f72");
}

TEST(MD5Tests, messageDigest) {
    EXPECT_EQ(runHashHex("message digest"), "f96b697d7cb7938d525a2f31aaf161d0");
}

// The classic 1,000,000 x 'a' RFC 1321 vector. ~15625 blocks, so it drives the
// bulk-update fast path (and its sub-block remainder handling) far past the
// single-/two-block cases.
TEST(MD5Tests, millionA) {
    EXPECT_EQ(runHashHexRepeated('a', 1000000),
              "7707d6ae4e027c70eea2a935c2296f21");
}

TEST(MD5Tests, alphabet) {
    EXPECT_EQ(runHashHex("abcdefghijklmnopqrstuvwxyz"),
              "c3fcd3d76192e4007dfb496cca67e13b");
}

TEST(MD5Tests, alphanumMixed) {
    EXPECT_EQ(runHashHex(
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"),
              "d174ab98d277d9f5a5611c2c9f419d9f");
}

TEST(MD5Tests, longDigitRun) {
    EXPECT_EQ(runHashHex(
        "1234567890123456789012345678901234567890"
        "1234567890123456789012345678901234567890"),
              "57edf4a22be3c955ac49da2e2107b67a");
}

TEST(MD5Tests, streamingMatchesOneShot) {
    auto src =
        "package test;\n"
        "import cajeta.hash.MD5;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] piece1 = heap int8[3];\n"
        "        piece1[0L] = (int8) 97; piece1[1L] = (int8) 98; piece1[2L] = (int8) 99;\n"
        "        int8[] piece2 = heap int8[4];\n"
        "        piece2[0L] = (int8) 100; piece2[1L] = (int8) 101;\n"
        "        piece2[2L] = (int8) 102; piece2[3L] = (int8) 103;\n"
        "        int8[] full = heap int8[7];\n"
        "        full[0L] = (int8) 97; full[1L] = (int8) 98; full[2L] = (int8) 99;\n"
        "        full[3L] = (int8) 100; full[4L] = (int8) 101;\n"
        "        full[5L] = (int8) 102; full[6L] = (int8) 103;\n"
        "        MD5 m = heap MD5();\n"
        "        m.update(piece1, 3L);\n"
        "        m.update(piece2, 4L);\n"
        "        int8[] streamed = m.digest();\n"
        "        int8[] oneshot = MD5.hash(full, 7L);\n"
        "        int32 i = 0;\n"
        "        while (i < 16) {\n"
        "            if (streamed[(int64) i] != oneshot[(int64) i]) { return 0; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

TEST(MD5Tests, finishProjectsFirst8Bytes) {
    auto src =
        "package test;\n"
        "import cajeta.hash.MD5;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int8[] data = heap int8[3];\n"
        "        data[0L] = (int8) 97; data[1L] = (int8) 98; data[2L] = (int8) 99;\n"
        "        MD5 m = heap MD5();\n"
        "        m.update(data, 3L);\n"
        "        return m.finish();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    int64_t expected = 0;
    uint8_t prefix[8] = {0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0};
    for (int i = 0; i < 8; i++) {
        expected |= ((int64_t)(uint64_t) prefix[i]) << (i * 8);
    }
    EXPECT_EQ(fn(), expected);
}

TEST(MD5Tests, resetClearsState) {
    auto src =
        "package test;\n"
        "import cajeta.hash.MD5;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] first = heap int8[3];\n"
        "        first[0L] = (int8) 97; first[1L] = (int8) 98; first[2L] = (int8) 99;\n"
        "        int8[] second = heap int8[1];\n"
        "        second[0L] = (int8) 97;\n"
        "        MD5 m = heap MD5();\n"
        "        m.update(first, 3L);\n"
        "        int8[] firstDigest = m.digest();\n"
        "        m.reset();\n"
        "        m.update(second, 1L);\n"
        "        int8[] secondDigest = m.digest();\n"
        "        int8[] reference = MD5.hash(second, 1L);\n"
        "        int32 i = 0;\n"
        "        while (i < 16) {\n"
        "            if (secondDigest[(int64) i] != reference[(int64) i]) { return 0; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
