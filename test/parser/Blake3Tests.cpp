// BLAKE3 tests. Pin against the official BLAKE3 test vectors (the canonical
// input pattern input[i] = i % 251), generated with the reference `blake3`
// crate. Exercises sub-chunk, chunk-boundary, multi-chunk-tree, and a large
// (1 MiB) tree, plus streaming==one-shot and the XOF (first 32 bytes == hash).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// JIT: hashHex of a `count`-byte buffer filled with the official i%251 pattern.
std::string b3hex(size_t count) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Blake3;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static pointer run() {\n"
        "        int64 n = " + std::to_string(count) + "L;\n"
        "        int8[] data = heap int8[n];\n"
        "        int64 i = 0L;\n"
        "        while (i < n) { data[i] = (int8) (i % 251L); i = i + 1L; }\n"
        "        String s = Blake3.hashHex(data, n);\n"
        "        return s.bytes;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    if (!hdr) return "<null>";
    int64_t c = *(int64_t*) hdr;
    return std::string((const char*) hdr + 8, (size_t) c);
}

} // namespace

TEST(Blake3Tests, empty) {
    EXPECT_EQ(b3hex(0),
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262");
}

TEST(Blake3Tests, subChunk3) {
    EXPECT_EQ(b3hex(3),
        "e1be4d7a8ab5560aa4199eea339849ba8e293d55ca0a81006726d184519e647f");
}

// One full chunk (1024) — single chunk, CHUNK_START|CHUNK_END, no tree.
TEST(Blake3Tests, oneChunk1024) {
    EXPECT_EQ(b3hex(1024),
        "42214739f095a406f3fc83deb889744ac00df831c10daa55189b5d121c855af7");
}

// 1025 bytes — two chunks, so a parent node: exercises the CV-stack merge.
TEST(Blake3Tests, twoChunkTree1025) {
    EXPECT_EQ(b3hex(1025),
        "d00278ae47eb27b34faecf67b4fe263f82d5412916c1ffd97c8cb7fb814b8444");
}

// 2049 bytes — three chunks, a deeper unbalanced tree.
TEST(Blake3Tests, threeChunkTree2049) {
    EXPECT_EQ(b3hex(2049),
        "5f4d72f40d7a5f82b15ca2b2e44b1de3c2ef86c426c95c1af0b6879522563030");
}

// 1 MiB — 1024 chunks, a full 10-deep tree (the bench size).
TEST(Blake3Tests, oneMiB) {
    EXPECT_EQ(b3hex(1048576),
        "74cb441fd087764ca9c3694da742ebe30cbeb3060a17009ca81825c7a8d10343");
}

// Streaming (two updates) must equal the one-shot hash over the concatenation.
TEST(Blake3Tests, streamingMatchesOneShot) {
    auto src =
        "package test;\n"
        "import cajeta.hash.Blake3;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 n = 3000L;\n"
        "        int8[] full = heap int8[n];\n"
        "        int64 i = 0L;\n"
        "        while (i < n) { full[i] = (int8) (i % 251L); i = i + 1L; }\n"
        "        int8[] p1 = heap int8[1100];\n"
        "        i = 0L; while (i < 1100L) { p1[i] = (int8) (i % 251L); i = i + 1L; }\n"
        "        int8[] p2 = heap int8[1900];\n"
        "        i = 0L; while (i < 1900L) { p2[i] = (int8) ((i + 1100L) % 251L); i = i + 1L; }\n"
        "        Blake3 h = heap Blake3();\n"
        "        h.update(p1, 1100L);\n"
        "        h.update(p2, 1900L);\n"
        "        int8[] streamed = h.digest();\n"
        "        int8[] oneshot = Blake3.hash(full, n);\n"
        "        int32 k = 0;\n"
        "        while (k < 32) {\n"
        "            if (streamed[(int64) k] != oneshot[(int64) k]) { return 0; }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// XOF: the first 32 bytes of extended output equal the 32-byte hash; pin the
// next 32 bytes to the reference (xof64 of the empty input).
TEST(Blake3Tests, xofExtendsHash) {
    auto src =
        "package test;\n"
        "import cajeta.hash.Blake3;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static pointer run() {\n"
        "        int8[] data = heap int8[0];\n"
        "        int8[] wide = heap int8[64];\n"
        "        Blake3.hashXof(data, 0L, wide);\n"
        "        int8[] hex = heap int8[128];\n"
        "        int8[] H = heap int8[16];\n"
        "        H[0L]=(int8)48; H[1L]=(int8)49; H[2L]=(int8)50; H[3L]=(int8)51;\n"
        "        H[4L]=(int8)52; H[5L]=(int8)53; H[6L]=(int8)54; H[7L]=(int8)55;\n"
        "        H[8L]=(int8)56; H[9L]=(int8)57; H[10L]=(int8)97; H[11L]=(int8)98;\n"
        "        H[12L]=(int8)99; H[13L]=(int8)100; H[14L]=(int8)101; H[15L]=(int8)102;\n"
        "        int64 j = 0L;\n"
        "        while (j < 64L) {\n"
        "            int64 b = ((int64) wide[j]) & 0xFFL;\n"
        "            hex[j * 2L] = H[b >>> 4L];\n"
        "            hex[j * 2L + 1L] = H[b & 0xFL];\n"
        "            j = j + 1L;\n"
        "        }\n"
        "        String s = heap String(#hex, 128);\n"
        "        return s.bytes;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    ASSERT_NE(hdr, nullptr);
    int64_t c = *(int64_t*) hdr;
    std::string out((const char*) hdr + 8, (size_t) c);
    EXPECT_EQ(out,
        "af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262"
        "e00f03e7b69af26b7faaf09fcd333050338ddfe085b8cc869ca98b206c08243a");
}
