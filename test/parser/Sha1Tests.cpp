// SHA-1 (FIPS 180-4) tests — NET-11.2.
//
// SHA-1 here exists ONLY for the WebSocket `Sec-WebSocket-Accept`
// handshake (RFC 6455 §1.3); it is flagged not-for-security in its
// doc-comment. These tests pin the implementation to the canonical
// FIPS 180-4 / RFC 3174 vectors plus the RFC 6455 handshake example.
//
// Vectors (lowercase hex):
//   ""                                  -> da39a3ee5e6b4b0d3255bfef95601890afd80709
//   "abc"                               -> a9993e364706816aba3e25717850c26c9cd0d89d
//   "abcdbcdecdefdefgefghfghighijhij"
//     "kijkljklmklmnlmnomnopnopq" (56B)  -> 84983e441c3bd26ebaae4aa1f95129e5e54670f1
//   RFC 6455 handshake input
//     "dGhlIHNhbXBsZSBub25jZQ=="
//     + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
//                                       -> b37a4f2cc0624f1690f64606cf385945b2bec4ea
//     (base64 of that digest is the spec's "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")
//
// The comparison is done inside the Cajeta program (digest bytes vs an
// expected byte array, returning 1/0) so the test never has to decode a
// cajeta String back on the host — mirroring SipHashTests' int-return
// style.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

// Build a Cajeta program that one-shot-hashes `input` and compares the
// 20-byte digest against `expected` (the canonical digest bytes),
// returning 1 on full match, else 0.
std::string buildOneShotMatchSrc(const std::string& input,
                                 const std::vector<uint8_t>& expected) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Sha1;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] data = heap int8[" + std::to_string(input.size() ? input.size() : 1) + "];\n";
    // For empty input we still allocate a 1-element array but hash 0 bytes.
    for (size_t i = 0; i < input.size(); i++) {
        src += "        data[" + std::to_string(i) + "L] = (int8) "
             + std::to_string((int)(uint8_t) input[i]) + ";\n";
    }
    src += "        int8[] digest #= Sha1.hash(data, " + std::to_string(input.size()) + "L);\n";
    src += "        int8[] expected = heap int8[20];\n";
    for (size_t i = 0; i < 20; i++) {
        src += "        expected[" + std::to_string(i) + "L] = (int8) "
             + std::to_string((int)(int8_t) expected[i]) + ";\n";
    }
    src +=
        "        int32 ok = 1;\n"
        "        for (int64 i = 0L; i < 20L; i = i + 1L) {\n"
        "            if (digest[i] != expected[i]) { ok = 0; }\n"
        "        }\n"
        "        return ok;\n"
        "    }\n"
        "}\n";
    return src;
}

int32_t runOneShotMatch(const std::string& input,
                        const std::vector<uint8_t>& expected) {
    auto src = buildOneShotMatchSrc(input, expected);
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Parse a lowercase hex string into raw bytes.
std::vector<uint8_t> hex(const std::string& h) {
    std::vector<uint8_t> out;
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
        return 0;
    };
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        out.push_back((uint8_t)((nyb(h[i]) << 4) | nyb(h[i + 1])));
    }
    return out;
}

} // namespace

// --- FIPS 180-4 / RFC 3174 canonical vectors -------------------------------

TEST(Sha1Tests, fipsVectors) {
    EXPECT_EQ(runOneShotMatch(
        "", hex("da39a3ee5e6b4b0d3255bfef95601890afd80709")), 1)
        << "SHA-1(\"\") mismatch";

    EXPECT_EQ(runOneShotMatch(
        "abc", hex("a9993e364706816aba3e25717850c26c9cd0d89d")), 1)
        << "SHA-1(\"abc\") mismatch";

    EXPECT_EQ(runOneShotMatch(
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        hex("84983e441c3bd26ebaae4aa1f95129e5e54670f1")), 1)
        << "SHA-1(56-byte msg) mismatch";
}

// --- RFC 6455 §1.3 WebSocket handshake vector ------------------------------
// The single reason this algorithm is in the tree.

TEST(Sha1Tests, rfc6455HandshakeDigest) {
    std::string in =
        "dGhlIHNhbXBsZSBub25jZQ=="
        "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    EXPECT_EQ(runOneShotMatch(
        in, hex("b37a4f2cc0624f1690f64606cf385945b2bec4ea")), 1)
        << "SHA-1 of RFC 6455 handshake input mismatch";
}

// --- Incremental (streaming) equals one-shot -------------------------------
// The handshake feeds the key and the GUID as two updates; assert the
// chunked digest equals the one-shot digest of the concatenation.

TEST(Sha1Tests, streamingMatchesOneShot) {
    auto src =
        "package test;\n"
        "import cajeta.hash.Sha1;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] piece1 = heap int8[3];\n"
        "        piece1[0L] = (int8) 97; piece1[1L] = (int8) 98; piece1[2L] = (int8) 99;\n" // "abc"
        "        int8[] piece2 = heap int8[3];\n"
        "        piece2[0L] = (int8) 100; piece2[1L] = (int8) 101; piece2[2L] = (int8) 102;\n" // "def"
        "        int8[] full = heap int8[6];\n"
        "        full[0L] = (int8) 97; full[1L] = (int8) 98; full[2L] = (int8) 99;\n"
        "        full[3L] = (int8) 100; full[4L] = (int8) 101; full[5L] = (int8) 102;\n"
        "        Sha1 h = heap Sha1();\n"
        "        h.update(piece1, 3L);\n"
        "        h.update(piece2, 3L);\n"
        "        int8[] streamed #= h.digest();\n"
        "        int8[] oneshot #= Sha1.hash(full, 6L);\n"
        "        int32 ok = 1;\n"
        "        for (int64 i = 0L; i < 20L; i = i + 1L) {\n"
        "            if (streamed[i] != oneshot[i]) { ok = 0; }\n"
        "        }\n"
        "        return ok;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// --- reset() re-initializes state so one instance hashes twice -------------

TEST(Sha1Tests, resetReusesInstance) {
    auto src =
        "package test;\n"
        "import cajeta.hash.Sha1;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] abc = heap int8[3];\n"
        "        abc[0L] = (int8) 97; abc[1L] = (int8) 98; abc[2L] = (int8) 99;\n"
        "        Sha1 h = heap Sha1();\n"
        "        h.update(abc, 3L);\n"
        "        int8[] first #= h.digest();\n"
        "        h.reset();\n"
        "        h.update(abc, 3L);\n"
        "        int8[] second #= h.digest();\n"
        "        int32 ok = 1;\n"
        "        for (int64 i = 0L; i < 20L; i = i + 1L) {\n"
        "            if (first[i] != second[i]) { ok = 0; }\n"
        "        }\n"
        "        return ok;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
