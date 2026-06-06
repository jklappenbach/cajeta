// SipHash-2-4 tests. Cross-verified against the veorq/SipHash C
// reference implementation (the canonical one referenced by the
// algorithm authors). Key = 00 01 02 03 04 05 06 07 08 09 0a 0b 0c
// 0d 0e 0f, inputs are the first N bytes of the same byte sequence.
//
// Expected values were produced by the veorq reference impl and
// re-verified by an independent compile of the same algorithm with
// 2 compression rounds + 4 finalization rounds. Two independent
// implementations agree.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int64_t runHashKeyed(const std::string& bytes,
                     int64_t k0, int64_t k1) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.SipHash;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int8[] data = heap int8[" + std::to_string(bytes.size()) + "];\n";
    for (size_t i = 0; i < bytes.size(); i++) {
        src += "        data[" + std::to_string(i) + "L] = (int8) "
             + std::to_string((int)(signed char) bytes[i]) + ";\n";
    }
    src += "        return SipHash.hashKeyed(data, " + std::to_string(bytes.size()) + "L, "
        + std::to_string(k0) + "L, " + std::to_string(k1) + "L);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

// k0 = 0x0706050403020100, k1 = 0x0f0e0d0c0b0a0908
static constexpr int64_t K0 = (int64_t) 0x0706050403020100LL;
static constexpr int64_t K1 = (int64_t) 0x0f0e0d0c0b0a0908LL;

TEST(SipHashTests, emptyInput) {
    EXPECT_EQ((uint64_t) runHashKeyed("", K0, K1),
              (uint64_t) 0x726fdb47dd0e0e31ULL);
}

TEST(SipHashTests, oneByte) {
    std::string b(1, '\0');
    EXPECT_EQ((uint64_t) runHashKeyed(b, K0, K1),
              (uint64_t) 0x74f839c593dc67fdULL);
}

TEST(SipHashTests, twoBytes) {
    std::string b;
    b.push_back('\0');
    b.push_back('\1');
    EXPECT_EQ((uint64_t) runHashKeyed(b, K0, K1),
              (uint64_t) 0x0d6c8009d9a94f5aULL);
}

TEST(SipHashTests, threeBytes) {
    std::string b;
    for (int i = 0; i < 3; i++) b.push_back((char) i);
    EXPECT_EQ((uint64_t) runHashKeyed(b, K0, K1),
              (uint64_t) 0x85676696d7fb7e2dULL);
}

TEST(SipHashTests, eightBytes) {
    std::string b;
    for (int i = 0; i < 8; i++) b.push_back((char) i);
    EXPECT_EQ((uint64_t) runHashKeyed(b, K0, K1),
              (uint64_t) 0x93f5f5799a932462ULL);
}

TEST(SipHashTests, fifteenBytes) {
    std::string b;
    for (int i = 0; i < 15; i++) b.push_back((char) i);
    // Pinned to my-impl + veorq-reference cross-check.
    EXPECT_EQ((uint64_t) runHashKeyed(b, K0, K1),
              (uint64_t) 0xa129ca6149be45e5ULL);
}

// Streaming Hasher must match one-shot for the same input + key.
TEST(SipHashTests, streamingMatchesOneShot) {
    auto src =
        "package test;\n"
        "import cajeta.hash.SipHash;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] piece1 = heap int8[3];\n"
        "        piece1[0L] = (int8) 1; piece1[1L] = (int8) 2; piece1[2L] = (int8) 3;\n"
        "        int8[] piece2 = heap int8[4];\n"
        "        piece2[0L] = (int8) 4; piece2[1L] = (int8) 5;\n"
        "        piece2[2L] = (int8) 6; piece2[3L] = (int8) 7;\n"
        "        int8[] full = heap int8[7];\n"
        "        full[0L] = (int8) 1; full[1L] = (int8) 2; full[2L] = (int8) 3;\n"
        "        full[3L] = (int8) 4; full[4L] = (int8) 5;\n"
        "        full[5L] = (int8) 6; full[6L] = (int8) 7;\n"
        "        SipHash h = heap SipHash(7L, 9L);\n"
        "        h.update(piece1, 3L);\n"
        "        h.update(piece2, 4L);\n"
        "        int64 streamed = h.finish();\n"
        "        int64 oneshot = SipHash.hashKeyed(full, 7L, 7L, 9L);\n"
        "        return (streamed == oneshot) ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// Different keys with the same bytes must produce different digests
// — the keyed-PRF property that makes SipHash DoS-resistant.
TEST(SipHashTests, keySensitivity) {
    auto src =
        "package test;\n"
        "import cajeta.hash.SipHash;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] data = heap int8[5];\n"
        "        data[0L] = (int8) 65; data[1L] = (int8) 66;\n"
        "        data[2L] = (int8) 67; data[3L] = (int8) 68; data[4L] = (int8) 69;\n"
        "        int64 h1 = SipHash.hashKeyed(data, 5L, 1L, 2L);\n"
        "        int64 h2 = SipHash.hashKeyed(data, 5L, 1L, 3L);\n"
        "        return (h1 != h2) ? 1 : 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
