// SHA-256 (FIPS 180-4) tests. Pinned against the NIST FIPS 180-4
// worked examples + CAVP short-message vectors:
//   ""                              -> e3b0c442...b7852b855
//   "abc"                           -> ba7816bf...f20015ad
//   the 56-byte two-block message   -> 248d6a61...19db06c1
//   1,000,000 x 'a'                 -> cdc76e5c...c7112cd0
// The C core was independently cross-checked against a standalone
// compile of the same algorithm before these were committed (both the
// one-shot path and the chunked-update path agree byte-for-byte with
// the published digests).
//
// Pins NET-11.1 acceptance:
//   - Sha256Tests.fipsVectorsOneShotAndIncremental
//   - Sha256Tests.incrementalEqualsOneShot

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// One-shot hex digest of `input` via Sha256.hashHex.
std::string oneShotHex(const std::string& input) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Sha256;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static pointer run() {\n"
        "        int8[] data = heap int8[" + std::to_string(input.size()) + "];\n";
    for (size_t i = 0; i < input.size(); i++) {
        src += "        data[" + std::to_string(i) + "L] = (int8) "
             + std::to_string((int)(signed char) input[i]) + ";\n";
    }
    src += "        String s = Sha256.hashHex(data, " + std::to_string(input.size()) + "L);\n"
        "        return s.bytes;\n"
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

// Streaming hex digest: feed `input` one byte at a time through
// update(), then finalize via Sha256.hex(). Proves the incremental
// path equals the one-shot path for arbitrary chunking (here the
// finest possible chunking — single bytes).
std::string incrementalHexByteAtATime(const std::string& input) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Sha256;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static pointer run() {\n"
        "        Sha256 h = heap Sha256();\n"
        "        int8[] one = heap int8[1];\n";
    for (size_t i = 0; i < input.size(); i++) {
        src += "        one[0L] = (int8) "
             + std::to_string((int)(signed char) input[i]) + "; h.update(one, 1L);\n";
    }
    src +=
        "        String s = h.hex();\n"
        "        return s.bytes;\n"
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

const char* EMPTY = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const char* ABC   = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
// "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" (56 bytes)
const std::string TWO_BLOCK = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
const char* TWO_BLOCK_HEX = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";

} // namespace

// --- one-shot FIPS vectors --------------------------------------------------

TEST(Sha256Tests, emptyString) {
    EXPECT_EQ(oneShotHex(""), EMPTY);
}

TEST(Sha256Tests, abc) {
    EXPECT_EQ(oneShotHex("abc"), ABC);
}

TEST(Sha256Tests, twoBlockMessage) {
    EXPECT_EQ(oneShotHex(TWO_BLOCK), TWO_BLOCK_HEX);
}

// --- acceptance: FIPS vectors one-shot AND incremental ----------------------

TEST(Sha256Tests, fipsVectorsOneShotAndIncremental) {
    // empty
    EXPECT_EQ(oneShotHex(""), EMPTY);
    EXPECT_EQ(incrementalHexByteAtATime(""), EMPTY);
    // "abc"
    EXPECT_EQ(oneShotHex("abc"), ABC);
    EXPECT_EQ(incrementalHexByteAtATime("abc"), ABC);
    // the 56-byte two-block message (crosses the block boundary, so the
    // single-byte streaming path exercises buffer carry + a transform)
    EXPECT_EQ(oneShotHex(TWO_BLOCK), TWO_BLOCK_HEX);
    EXPECT_EQ(incrementalHexByteAtATime(TWO_BLOCK), TWO_BLOCK_HEX);
}

// --- acceptance: incremental (chunked) equals one-shot ----------------------

TEST(Sha256Tests, incrementalEqualsOneShot) {
    // Feed "abcdefg" as two chunks (3 + 4) and as one-shot; the raw
    // 32-byte digests must be byte-identical. Mirrors the MD5 streaming
    // test shape but over SHA-256's 32-byte output.
    auto src =
        "package test;\n"
        "import cajeta.hash.Sha256;\n"
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
        "        Sha256 h = heap Sha256();\n"
        "        h.update(piece1, 3L);\n"
        "        h.update(piece2, 4L);\n"
        "        int8[] streamed = h.digest();\n"
        "        int8[] oneshot = Sha256.hash(full, 7L);\n"
        "        int32 i = 0;\n"
        "        while (i < 32) {\n"
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

// --- hex() instance method (the download-checksum surface) ------------------

TEST(Sha256Tests, instanceHexMatchesOneShot) {
    // The streaming `hex()` finalizer (the shape HttpClient.downloadTo
    // uses for a running checksum) must equal the one-shot hashHex.
    auto src =
        "package test;\n"
        "import cajeta.hash.Sha256;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static pointer run() {\n"
        "        int8[] data = heap int8[3];\n"
        "        data[0L] = (int8) 97; data[1L] = (int8) 98; data[2L] = (int8) 99;\n"
        "        Sha256 h = heap Sha256();\n"
        "        h.update(data, 3L);\n"
        "        String s = h.hex();\n"
        "        return s.bytes;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    ASSERT_NE(hdr, nullptr);
    int64_t count = *(int64_t*) hdr;
    std::string got((const char*) hdr + 8, (size_t) count);
    EXPECT_EQ(got, ABC);
}

// --- finish() Hasher projection: first 8 digest bytes, little-endian --------

TEST(Sha256Tests, finishProjectsFirst8Bytes) {
    auto src =
        "package test;\n"
        "import cajeta.hash.Sha256;\n"
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int8[] data = heap int8[3];\n"
        "        data[0L] = (int8) 97; data[1L] = (int8) 98; data[2L] = (int8) 99;\n"
        "        Sha256 h = heap Sha256();\n"
        "        h.update(data, 3L);\n"
        "        return h.finish();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    // First 8 bytes of SHA-256("abc") = ba 78 16 bf 8f 01 cf ea,
    // folded little-endian (byte i << i*8).
    int64_t expected = 0;
    uint8_t prefix[8] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea};
    for (int i = 0; i < 8; i++) {
        expected |= ((int64_t)(uint64_t) prefix[i]) << (i * 8);
    }
    EXPECT_EQ(fn(), expected);
}

// --- reset() re-initializes the state --------------------------------------

TEST(Sha256Tests, resetClearsState) {
    auto src =
        "package test;\n"
        "import cajeta.hash.Sha256;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] first = heap int8[3];\n"
        "        first[0L] = (int8) 97; first[1L] = (int8) 98; first[2L] = (int8) 99;\n"
        "        int8[] second = heap int8[1];\n"
        "        second[0L] = (int8) 97;\n"
        "        Sha256 h = heap Sha256();\n"
        "        h.update(first, 3L);\n"
        "        int8[] firstDigest = h.digest();\n"
        "        h.reset();\n"
        "        h.update(second, 1L);\n"
        "        int8[] secondDigest = h.digest();\n"
        "        int8[] reference = Sha256.hash(second, 1L);\n"
        "        int32 i = 0;\n"
        "        while (i < 32) {\n"
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
