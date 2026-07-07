// Base64 (RFC 4648) codec tests — NET-11.3.
//
// Pins cajeta.codec.Base64 against the canonical RFC 4648 §10 test
// vectors ("" .. "foobar"), the URL-safe (§5) alphabet, padding on/off,
// round-trips, and the two decode error paths (invalid character,
// length 1 mod 4). Needed by the WS handshake (Sec-WebSocket-Accept)
// and HTTP Basic auth.
//
// Pattern mirrors MD5Tests / Sha1Tests: encode returns a cajeta String
// whose bytes we read back on the host; decode-equality and error
// checks run inside the Cajeta program returning 1/0 so the host never
// has to round-trip a String for the byte comparison.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

// Emit Cajeta statements that materialize `input` into an int8[] named
// `data` of length max(N,1) (a zero-length `heap int8[0]` is avoided to
// keep the snippet uniform; the codec is always called with the true
// length).
std::string emitDataArray(const std::string& input) {
    size_t n = input.size();
    std::string src =
        "        int8[] data = heap int8["
        + std::to_string(n ? n : 1) + "];\n";
    for (size_t i = 0; i < n; i++) {
        src += "        data[" + std::to_string(i) + "L] = (int8) "
             + std::to_string((int)(signed char) input[i]) + ";\n";
    }
    return src;
}

// Encode `input` with the standard, padded alphabet and read the
// resulting String bytes back on the host.
std::string runEncode(const std::string& input) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base64;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        + emitDataArray(input) +
        "        String s = Base64.encode(data, "
        + std::to_string(input.size()) + "L);\n"
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

// Encode `input` URL-safe, unpadded.
std::string runEncodeUrlSafe(const std::string& input) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base64;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        + emitDataArray(input) +
        "        String s = Base64.encodeUrlSafe(data, "
        + std::to_string(input.size()) + "L);\n"
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

// Decode `encoded` and read the resulting raw bytes back on the host.
std::string runDecode(const std::string& encoded) {
    std::string lit;
    for (char c : encoded) {
        if (c == '\\' || c == '"') lit += '\\';
        lit += c;
    }
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base64;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        // decode returns an owned `#int8[]`; the run() signature must carry
        // the `#` and the return must use the transfer operator, else the
        // borrow checker rejects it (CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER).
        "    public static #int8[] run() {\n"
        "        int8[] raw = Base64.decode(\"" + lit + "\");\n"
        "        return #raw;\n"
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

} // namespace

// --- RFC 4648 §10 encode vectors (standard, padded) ------------------

TEST(Base64Tests, encodeEmpty)       { EXPECT_EQ(runEncode(""),       ""); }
TEST(Base64Tests, encodeF)           { EXPECT_EQ(runEncode("f"),      "Zg=="); }
TEST(Base64Tests, encodeFo)          { EXPECT_EQ(runEncode("fo"),     "Zm8="); }
TEST(Base64Tests, encodeFoo)         { EXPECT_EQ(runEncode("foo"),    "Zm9v"); }
TEST(Base64Tests, encodeFoob)        { EXPECT_EQ(runEncode("foob"),   "Zm9vYg=="); }
TEST(Base64Tests, encodeFooba)       { EXPECT_EQ(runEncode("fooba"),  "Zm9vYmE="); }
TEST(Base64Tests, encodeFoobar)      { EXPECT_EQ(runEncode("foobar"), "Zm9vYmFy"); }

// Alphabet coverage: 0xFF 0xFF 0xFF -> "////" exercises index 63; the
// "+/'" producing inputs exercise 62/63 on the standard alphabet.
TEST(Base64Tests, encodeAllOnes) {
    EXPECT_EQ(runEncode(std::string("\xff\xff\xff", 3)), "////");
}
TEST(Base64Tests, encodePlusSlash) {
    // 0xFB 0xF0 -> "+/A=" (sextets 62, 63, 0 then pad).
    EXPECT_EQ(runEncode(std::string("\xfb\xf0", 2)), "+/A=");
}

// --- URL-safe (§5), unpadded -----------------------------------------

TEST(Base64Tests, encodeUrlSafeNoPad) {
    // Same bytes that produce "+/A=" standard become "-_A" url-safe,
    // unpadded.
    EXPECT_EQ(runEncodeUrlSafe(std::string("\xfb\xf0", 2)), "-_A");
}
TEST(Base64Tests, encodeUrlSafeFoob) {
    // "foob" standard is "Zm9vYg==" -> url-safe unpadded drops the pad.
    EXPECT_EQ(runEncodeUrlSafe("foob"), "Zm9vYg");
}
TEST(Base64Tests, encodeUrlSafeIndex62And63) {
    // 0xFB 0xFF 0xBF -> sextets 62,63,62,63 -> "-_-_".
    EXPECT_EQ(runEncodeUrlSafe(std::string("\xfb\xff\xbf", 3)), "-_-_");
}

// --- Decode round-trips (both alphabets, padded + unpadded) ----------

TEST(Base64Tests, decodeEmpty)   { EXPECT_EQ(runDecode(""),         ""); }
TEST(Base64Tests, decodeF)       { EXPECT_EQ(runDecode("Zg=="),     "f"); }
TEST(Base64Tests, decodeFo)      { EXPECT_EQ(runDecode("Zm8="),     "fo"); }
TEST(Base64Tests, decodeFoo)     { EXPECT_EQ(runDecode("Zm9v"),     "foo"); }
TEST(Base64Tests, decodeFoob)    { EXPECT_EQ(runDecode("Zm9vYg=="), "foob"); }
TEST(Base64Tests, decodeFooba)   { EXPECT_EQ(runDecode("Zm9vYmE="), "fooba"); }
TEST(Base64Tests, decodeFoobar)  { EXPECT_EQ(runDecode("Zm9vYmFy"), "foobar"); }

// Unpadded standard input still decodes (padding is reconstructed from
// the length).
TEST(Base64Tests, decodeUnpadded) {
    EXPECT_EQ(runDecode("Zm9vYg"), "foob");
    EXPECT_EQ(runDecode("Zm9vYmE"), "fooba");
}

// URL-safe characters decode transparently.
TEST(Base64Tests, decodeUrlSafeAlphabet) {
    EXPECT_EQ(runDecode("-_-_"), std::string("\xfb\xff\xbf", 3));
    EXPECT_EQ(runDecode("-_A"),  std::string("\xfb\xf0", 2));
}

// --- Round-trip property (binary bytes, all 256 values) --------------

TEST(Base64Tests, roundTripAllByteValues) {
    // Build a 256-byte buffer of every byte value, encode then decode
    // inside Cajeta, and verify the bytes survive. Returns 1 on success.
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base64;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] data = heap int8[256];\n"
        "        int32 i = 0;\n"
        "        while (i < 256) { data[(int64) i] = (int8) i; i = i + 1; }\n"
        "        String enc = Base64.encode(data, 256L);\n"
        "        int8[] back = Base64.decode(enc);\n"
        "        if (back.count() != 256L) { return 0; }\n"
        "        i = 0;\n"
        "        while (i < 256) {\n"
        "            if (back[(int64) i] != data[(int64) i]) { return 0; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// --- Named acceptance test (plan NET-11.3) ---------------------------

// Round-trips a corpus of every length 0..96 (so every residue mod 3 →
// every padding form: 0, 1, 2 trailing bytes) through BOTH the standard
// and the URL-safe alphabets, verifying byte-exact recovery. The
// per-length payload cycles all 256 byte values so the alphabet's full
// index range (incl. 62/63 = +///-_) is exercised. Returns 1 on full
// success.
//
// This is the criterion named in plan/cajeta-net-plan.md:
//   "Base64 round-trips a fuzz corpus (incl. all padding lengths) for
//    both alphabets." -> Base64Tests.roundTripStdAndUrlSafe
TEST(Base64Tests, roundTripStdAndUrlSafe) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base64;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 len = 0;\n"
        "        while (len <= 96) {\n"
        "            int8[] data = heap int8[len];\n"
        "            int32 j = 0;\n"
        "            while (j < len) {\n"
        "                data[(int64) j] = (int8) ((j * 7 + 3) & 0xff);\n"
        "                j = j + 1;\n"
        "            }\n"
        // Standard, padded.
        "            String enc = Base64.encode(data, (int64) len);\n"
        "            int8[] back = Base64.decode(enc);\n"
        "            if (back.count() != (int64) len) { return 0; }\n"
        "            j = 0;\n"
        "            while (j < len) {\n"
        "                if (back[(int64) j] != data[(int64) j]) { return 0; }\n"
        "                j = j + 1;\n"
        "            }\n"
        // URL-safe, unpadded.
        "            String encU = Base64.encodeUrlSafe(data, (int64) len);\n"
        "            int8[] backU = Base64.decode(encU);\n"
        "            if (backU.count() != (int64) len) { return 0; }\n"
        "            j = 0;\n"
        "            while (j < len) {\n"
        "                if (backU[(int64) j] != data[(int64) j]) { return 0; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            len = len + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// --- Decode error paths ----------------------------------------------

// A character outside the alphabet (and not '=') throws Base64Exception.
// The Cajeta program catches it and returns 1; a missing throw returns 0.
TEST(Base64Tests, decodeInvalidCharThrows) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base64;\n"
        "import cajeta.codec.Base64Exception;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            int8[] raw = Base64.decode(\"aa!a\");\n"
        "            return 0;\n"
        "        } catch (Base64Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}

// A stripped length of 1 mod 4 ("Zm9vY" -> 5 chars) is a truncated
// final quantum and throws.
TEST(Base64Tests, decodeTruncatedQuantumThrows) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base64;\n"
        "import cajeta.codec.Base64Exception;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        try {\n"
        "            int8[] raw = Base64.decode(\"Zm9vY\");\n"
        "            return 0;\n"
        "        } catch (Base64Exception e) {\n"
        "            return 1;\n"
        "        }\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 1);
}
