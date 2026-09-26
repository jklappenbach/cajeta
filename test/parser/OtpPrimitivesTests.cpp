// HMAC-SHA1 (RFC 2104, vectors from RFC 2202), HOTP (RFC 4226 appendix D),
// TOTP-SHA1 (RFC 6238 appendix B), and the Base32 codec (RFC 4648 §10).
// cajeta-cloud-identity plan 3.1.13: what authenticator apps need.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

std::string bytesDecl(const std::string& name, const std::vector<uint8_t>& v) {
    std::string s = "        int8[] " + name + " = heap int8[" + std::to_string(v.size() == 0 ? 1 : v.size()) + "];\n";
    for (size_t i = 0; i < v.size(); i++) {
        s += "        " + name + "[" + std::to_string(i) + "L] = (int8) "
           + std::to_string((int)(int8_t) v[i]) + ";\n";
    }
    return s;
}

std::vector<uint8_t> repeated(uint8_t b, size_t n) { return std::vector<uint8_t>(n, b); }
std::vector<uint8_t> ascii(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

std::vector<uint8_t> runBytes(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    if (!hdr) return {};
    int64_t count = *(int64_t*) hdr;
    const uint8_t* data = (const uint8_t*) hdr + 8;
    return std::vector<uint8_t>(data, data + count);
}

std::string text(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

const char* kPutBytes =
    "    static void put(int8[] out, int64 at, int8[] src, int64 n) {\n"
    "        int64 i = 0;\n"
    "        while (i < n) { out[at + i] = src[i]; i = i + 1; }\n"
    "    }\n";

struct HmacCase { std::vector<uint8_t> key; std::vector<uint8_t> data; std::string mac; };

std::vector<HmacCase> rfc2202() {
    std::vector<uint8_t> key4;
    for (int i = 1; i <= 25; i++) key4.push_back((uint8_t) i);
    return {
        { repeated(0x0b, 20), ascii("Hi There"), "b617318655057264e28bc0b6fb378c8ef146be00" },
        { ascii("Jefe"), ascii("what do ya want for nothing?"), "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79" },
        { repeated(0xaa, 20), repeated(0xdd, 50), "125d7342b9ac11cd91a39af48aa17b4f63f175d3" },
        { key4, repeated(0xcd, 50), "4c9007f4026250c6bc8414f9bf50c86c2d7235da" },
        { repeated(0x0c, 20), ascii("Test With Truncation"), "4c1a03424b55e07fe7f27be1d58bb9324a9a5a04" },
        { repeated(0xaa, 80), ascii("Test Using Larger Than Block-Size Key - Hash Key First"),
          "aa4ae5e15272d00e95705637ce8a3b55ed402112" },
        { repeated(0xaa, 80), ascii("Test Using Larger Than Block-Size Key and Larger Than One Block-Size Data"),
          "e8e99d0f45237d786d6bbaa7965c7808bbff1a91" },
    };
}

std::string hmacSha1Src(const std::vector<HmacCase>& cases, bool streaming) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.HmacSha1;\n"
        "public final class D {\n" + std::string(kPutBytes) +
        "    public static #int8[] run() {\n"
        "        int8[] out = heap int8[" + std::to_string(20 * cases.size()) + "];\n";
    for (size_t n = 0; n < cases.size(); n++) {
        const auto& c = cases[n];
        std::string k = "key" + std::to_string(n), d = "data" + std::to_string(n), m = "mac" + std::to_string(n);
        src += bytesDecl(k, c.key) + bytesDecl(d, c.data);
        std::string kl = std::to_string(c.key.size()) + "L", dl = std::to_string(c.data.size()) + "L";
        if (streaming) {
            std::string half = std::to_string(c.data.size() / 2) + "L";
            std::string h = "h" + std::to_string(n), r = "rest" + std::to_string(n), j = "j" + std::to_string(n);
            src +=
            "        HmacSha1 " + h + " = heap HmacSha1(" + k + ", " + kl + ");\n"
            "        " + h + ".update(" + d + ", " + half + ");\n"
            "        int8[] " + r + " = heap int8[" + std::to_string(c.data.size() - c.data.size() / 2 + 1) + "];\n"
            "        int64 " + j + " = 0;\n"
            "        while (" + half + " + " + j + " < " + dl + ") { " + r + "[" + j + "] = " + d + "[" + half + " + " + j + "]; " + j + " = " + j + " + 1; }\n"
            "        " + h + ".update(" + r + ", " + j + ");\n"
            "        int8[] " + m + " #= " + h + ".digest();\n";
        } else {
            src += "        int8[] " + m + " #= HmacSha1.mac(" + k + ", " + kl + ", " + d + ", " + dl + ");\n";
        }
        src += "        D.put(out, " + std::to_string(20 * n) + "L, " + m + ", 20L);\n";
    }
    src += "        return #out;\n    }\n}\n";
    return src;
}

void expectAllMacs(const std::vector<HmacCase>& cases, const std::vector<uint8_t>& out) {
    ASSERT_EQ(out.size(), 20 * cases.size());
    for (size_t n = 0; n < cases.size(); n++) {
        std::vector<uint8_t> one(out.begin() + 20 * n, out.begin() + 20 * (n + 1));
        EXPECT_EQ(hex(one), cases[n].mac) << "RFC 2202 case " << (n + 1);
    }
}

// Codes written as ASCII digits, so zero padding is tested with the value.
std::string hotpSrc(const std::vector<int64_t>& counters, int digits) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Totp;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n" + std::string(kPutBytes) +
        "    public static #int8[] run() {\n"
        "        int8[] out = heap int8[" + std::to_string(digits * counters.size()) + "];\n"
        + bytesDecl("secret", ascii("12345678901234567890"));
    for (size_t n = 0; n < counters.size(); n++) {
        std::string s = "s" + std::to_string(n), b = "b" + std::to_string(n);
        src += "        String " + s + " #= Totp.format(Totp.hotp(secret, 20L, " + std::to_string(counters[n]) + "L, " + std::to_string(digits) + "), " + std::to_string(digits) + ");\n"
               "        int8[] " + b + " #= " + s + ".toBytes();\n"
               "        D.put(out, " + std::to_string(digits * n) + "L, " + b + ", " + std::to_string(digits) + "L);\n";
    }
    src += "        return #out;\n    }\n}\n";
    return src;
}

std::string totpSrc(const std::vector<int64_t>& times, int digits) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Totp;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n" + std::string(kPutBytes) +
        "    public static #int8[] run() {\n"
        "        int8[] out = heap int8[" + std::to_string(digits * times.size()) + "];\n"
        + bytesDecl("secret", ascii("12345678901234567890"));
    for (size_t n = 0; n < times.size(); n++) {
        std::string s = "s" + std::to_string(n), b = "b" + std::to_string(n);
        src += "        String " + s + " #= Totp.format(Totp.code(secret, 20L, " + std::to_string(times[n]) + "L, 30, " + std::to_string(digits) + "), " + std::to_string(digits) + ");\n"
               "        int8[] " + b + " #= " + s + ".toBytes();\n"
               "        D.put(out, " + std::to_string(digits * n) + "L, " + b + ", " + std::to_string(digits) + "L);\n";
    }
    src += "        return #out;\n    }\n}\n";
    return src;
}

std::string base32EncodeSrc(bool pad) {
    const char* words[] = { "", "f", "fo", "foo", "foob", "fooba", "foobar" };
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base32;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        "        String acc = \"\";\n";
    for (int n = 0; n < 7; n++) {
        std::string w = "w" + std::to_string(n), e = "e" + std::to_string(n);
        src += bytesDecl(w, ascii(words[n]));
        src += "        String " + e + " #= Base32.encode(" + w + ", " + std::to_string(std::string(words[n]).size()) + "L, " + (pad ? "true" : "false") + ");\n"
               "        acc = acc + \"|\" + " + e + ";\n";
    }
    src += "        return acc.toBytes();\n    }\n}\n";
    return src;
}

}  // namespace

TEST(HmacSha1Tests, rfc2202VectorsOneShot) {
    auto cases = rfc2202();
    expectAllMacs(cases, runBytes(hmacSha1Src(cases, false)));
}

TEST(HmacSha1Tests, rfc2202VectorsStreaming) {
    auto cases = rfc2202();
    expectAllMacs(cases, runBytes(hmacSha1Src(cases, true)));
}

TEST(HotpTests, rfc4226AppendixDCounters0To9) {
    EXPECT_EQ(text(runBytes(hotpSrc({0, 1, 2, 3, 4, 5, 6, 7, 8, 9}, 6))),
              "755224287082359152969429338314254676287922162583399871520489");
}

TEST(TotpTests, rfc6238AppendixBSha1) {
    EXPECT_EQ(text(runBytes(totpSrc({59, 1111111109, 1111111111, 1234567890, 2000000000, 20000000000LL}, 8))),
              "942870820708180414050471890059246927903765353130");
}

TEST(TotpTests, sixDigitsIsTheAuthenticatorDefaultAndZeroPads) {
    // Counter 3 of RFC 4226 appendix D is 1726969429, so 26969429 at 8 digits and 969429 at 6.
    EXPECT_EQ(text(runBytes(hotpSrc({3}, 8))), "26969429");
    EXPECT_EQ(text(runBytes(hotpSrc({3}, 6))), "969429");
    // RFC 6238's second vector begins with a zero and format must keep it.
    EXPECT_EQ(text(runBytes(totpSrc({1111111109}, 8))), "07081804");
}

TEST(TotpTests, otpauthUriCarriesTheBase32SecretAndEscapes) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Totp;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        + bytesDecl("secret", ascii("12345678901234567890")) +
        "        String u #= Totp.uri(\"Ex Co\", \"ada@example.test\", secret, 20L);\n"
        "        return u.toBytes();\n"
        "    }\n}\n";
    EXPECT_EQ(text(runBytes(src)),
        "otpauth://totp/Ex%20Co:ada%40example.test?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
        "&issuer=Ex%20Co&algorithm=SHA1&digits=6&period=30");
}

TEST(Base32Tests, rfc4648Section10Padded) {
    EXPECT_EQ(text(runBytes(base32EncodeSrc(true))),
              "||MY======|MZXQ====|MZXW6===|MZXW6YQ=|MZXW6YTB|MZXW6YTBOI======");
}

TEST(Base32Tests, rfc4648Section10Unpadded) {
    EXPECT_EQ(text(runBytes(base32EncodeSrc(false))),
              "||MY|MZXQ|MZXW6|MZXW6YQ|MZXW6YTB|MZXW6YTBOI");
}

TEST(Base32Tests, decodeAcceptsPaddedUnpaddedAndLowercase) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base32;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        "        int8[] a #= Base32.decode(\"MZXW6YQ=\");\n"
        "        int8[] b #= Base32.decode(\"MZXW6YTBOI\");\n"
        "        int8[] c #= Base32.decode(\"mzxw6ytboi======\");\n"
        "        int8[] d #= Base32.decode(\"\");\n"
        "        String s = heap String(#a, 4) + heap String(#b, 6) + heap String(#c, 6) + \"|\" + (int64) d.count();\n"
        "        return s.toBytes();\n"
        "    }\n}\n";
    EXPECT_EQ(text(runBytes(src)), "foobfoobarfoobar|0");
}

TEST(Base32Tests, decodeRejectsForeignCharactersAndImpossibleLengths) {
    std::string src =
        "package test;\n"
        "import cajeta.codec.Base32;\n"
        "import cajeta.codec.Base32Exception;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    static #String probe(String s) {\n"
        "        try {\n"
        "            int8[] never #= Base32.decode(s);\n"
        "        } catch (Base32Exception e) {\n"
        "            String out = \"E@\" + e.position;\n"
        "            return #out;\n"
        "        }\n"
        "        String ok = \"ok\";\n"
        "        return #ok;\n"
        "    }\n"
        "    public static #int8[] run() {\n"
        "        String s #= D.probe(\"MZ1W\") + \"|\" + D.probe(\"M\") + \"|\" + D.probe(\"MZXW6YTB\");\n"
        "        return s.toBytes();\n"
        "    }\n}\n";
    EXPECT_EQ(text(runBytes(src)), "E@2|E@1|ok");
}
