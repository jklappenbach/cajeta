// The Base32 codec (RFC 4648 §10), cajeta.codec.Base32: it stays in the stdlib
// when the cryptographic types move to dev.cajeta.crypto (cajeta-crypto spec
// 6.3), so its cases stay here; OtpPrimitivesTests' HMAC-SHA1, HOTP and TOTP
// cases moved with those types.

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
