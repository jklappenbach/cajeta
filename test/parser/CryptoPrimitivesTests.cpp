// HMAC-SHA256 (RFC 2104, vectors from RFC 4231), PBKDF2-HMAC-SHA256 (RFC
// 8018, vectors from RFC 7914 §11), the CSPRNG over the native entropy path,
// and the constant-time byte compare. cajeta-cloud-identity plan Unit 0.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <set>
#include <sstream>
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

// Runs `#int8[] run()` and returns the array's bytes.
std::vector<uint8_t> runBytes(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<void* (*)()>("run");
    void* hdr = fn();
    if (!hdr) return {};
    int64_t count = *(int64_t*) hdr;
    const uint8_t* data = (const uint8_t*) hdr + 8;
    return std::vector<uint8_t>(data, data + count);
}

std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

struct HmacCase { std::vector<uint8_t> key; std::vector<uint8_t> data; std::string mac; };

// One program computing every case's MAC into a 32*N byte array, so the
// suite pays one JIT compile per test rather than one per vector.
std::string hmacSrc(const std::vector<HmacCase>& cases, bool streaming) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.HmacSha256;\n"
        "public final class D {\n"
        "    static void put(int8[] out, int64 at, int8[] mac) {\n"
        "        int64 i = 0;\n"
        "        while (i < 32L) { out[at + i] = mac[i]; i = i + 1; }\n"
        "    }\n"
        "    public static #int8[] run() {\n"
        "        int8[] out = heap int8[" + std::to_string(32 * cases.size()) + "];\n";
    for (size_t n = 0; n < cases.size(); n++) {
        const auto& c = cases[n];
        std::string k = "key" + std::to_string(n), d = "data" + std::to_string(n);
        std::string m = "mac" + std::to_string(n);
        src += bytesDecl(k, c.key) + bytesDecl(d, c.data);
        std::string kl = std::to_string(c.key.size()) + "L";
        std::string dl = std::to_string(c.data.size()) + "L";
        if (streaming) {
            std::string half = std::to_string(c.data.size() / 2) + "L";
            std::string h = "h" + std::to_string(n), r = "rest" + std::to_string(n), j = "j" + std::to_string(n);
            src +=
            "        HmacSha256 " + h + " = heap HmacSha256(" + k + ", " + kl + ");\n"
            "        " + h + ".update(" + d + ", " + half + ");\n"
            "        int8[] " + r + " = heap int8[" + std::to_string(c.data.size() - c.data.size() / 2 + 1) + "];\n"
            "        int64 " + j + " = 0;\n"
            "        while (" + half + " + " + j + " < " + dl + ") { " + r + "[" + j + "] = " + d + "[" + half + " + " + j + "]; " + j + " = " + j + " + 1; }\n"
            "        " + h + ".update(" + r + ", " + j + ");\n"
            "        int8[] " + m + " #= " + h + ".digest();\n";
        } else {
            src += "        int8[] " + m + " #= HmacSha256.mac(" + k + ", " + kl + ", " + d + ", " + dl + ");\n";
        }
        src += "        D.put(out, " + std::to_string(32 * n) + "L, " + m + ");\n";
    }
    src += "        return #out;\n    }\n}\n";
    return src;
}

std::vector<HmacCase> rfc4231() {
    std::vector<uint8_t> key4;
    for (int i = 1; i <= 25; i++) key4.push_back((uint8_t) i);
    return {
        { repeated(0x0b, 20), ascii("Hi There"),
          "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7" },
        { ascii("Jefe"), ascii("what do ya want for nothing?"),
          "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843" },
        { repeated(0xaa, 20), repeated(0xdd, 50),
          "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe" },
        { key4, repeated(0xcd, 50),
          "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b" },
        { repeated(0x0c, 20), ascii("Test With Truncation"),
          "a3b6167473100ee06e0c796c2955552bfa6f7c0a6a8aef8b93f860aab0cd20c5" },
        { repeated(0xaa, 131), ascii("Test Using Larger Than Block-Size Key - Hash Key First"),
          "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54" },
        { repeated(0xaa, 131), ascii("This is a test using a larger than block-size key and a larger than block-size data. The key needs to be hashed before being used by the HMAC algorithm."),
          "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2" },
    };
}

std::string pbkdf2Src(const std::string& password, const std::string& salt, int iterations, int length) {
    return
        "package test;\n"
        "import cajeta.hash.Pbkdf2;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        + bytesDecl("p", ascii(password)) + bytesDecl("s", ascii(salt)) +
        "        int8[] out #= Pbkdf2.sha256(p, " + std::to_string(password.size()) + "L, s, "
        + std::to_string(salt.size()) + "L, " + std::to_string(iterations) + ", " + std::to_string(length) + ");\n"
        "        return #out;\n"
        "    }\n}\n";
}

// Function body text for the first `define` whose header contains `symbol`.
std::string functionBody(const std::string& ir, const std::string& symbol) {
    size_t def = ir.find("define");
    while (def != std::string::npos) {
        size_t nl = ir.find('\n', def);
        std::string header = ir.substr(def, nl - def);
        if (header.find(symbol) != std::string::npos) {
            size_t end = ir.find("\n}", nl);
            return ir.substr(nl, (end == std::string::npos ? ir.size() : end) - nl);
        }
        def = ir.find("define", nl);
    }
    return "";
}

// True iff some conditional branch in `body` depends on a value derived from
// an i8 load. Tracks taint forward through SSA names in program order, with a
// second pass so phi back-edges are seen.
bool branchesOnLoadedData(const std::string& body) {
    std::vector<std::string> lines;
    { std::istringstream in(body); std::string l; while (std::getline(in, l)) lines.push_back(l); }
    std::set<std::string> tainted;
    auto defName = [](const std::string& l) -> std::string {
        size_t eq = l.find(" = ");
        if (eq == std::string::npos) return "";
        size_t start = l.find_first_not_of(' ');
        return l.substr(start, eq - start);
    };
    auto mentionsTainted = [&](const std::string& l) {
        for (const auto& t : tainted) {
            size_t p = l.find(t);
            while (p != std::string::npos) {
                char after = p + t.size() < l.size() ? l[p + t.size()] : ' ';
                if (!(isalnum((unsigned char) after) || after == '_' || after == '.')) return true;
                p = l.find(t, p + 1);
            }
        }
        return false;
    };
    for (int pass = 0; pass < 2; pass++) {
        for (const auto& l : lines) {
            std::string name = defName(l);
            if (name.empty()) continue;
            if (l.find(" = load i8") != std::string::npos) { tainted.insert(name); continue; }
            std::string rhs = l.substr(l.find(" = ") + 3);
            if (mentionsTainted(rhs)) tainted.insert(name);
        }
    }
    for (const auto& l : lines) {
        size_t br = l.find("br i1 ");
        if (br == std::string::npos) continue;
        size_t comma = l.find(',', br);
        std::string cond = l.substr(br + 6, comma - (br + 6));
        if (tainted.count(cond)) return true;
    }
    return false;
}

} // namespace

void expectAllMacs(const std::vector<HmacCase>& cases, const std::vector<uint8_t>& out) {
    ASSERT_EQ(out.size(), 32 * cases.size());
    for (size_t n = 0; n < cases.size(); n++) {
        std::vector<uint8_t> one(out.begin() + 32 * n, out.begin() + 32 * (n + 1));
        EXPECT_EQ(hex(one), cases[n].mac) << "RFC 4231 case " << (n + 1);
    }
}

TEST(HmacSha256Tests, rfc4231VectorsOneShot) {
    auto cases = rfc4231();
    expectAllMacs(cases, runBytes(hmacSrc(cases, false)));
}

TEST(HmacSha256Tests, rfc4231VectorsStreaming) {
    auto cases = rfc4231();
    expectAllMacs(cases, runBytes(hmacSrc(cases, true)));
}

TEST(HmacSha256Tests, resetReusesTheKey) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.HmacSha256;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        + bytesDecl("key", ascii("Jefe")) + bytesDecl("a", ascii("first")) + bytesDecl("b", ascii("what do ya want for nothing?")) +
        "        HmacSha256 h = heap HmacSha256(key, 4L);\n"
        "        h.update(a, 5L);\n"
        "        int8[] first #= h.digest();\n"
        "        h.reset();\n"
        "        h.update(b, 28L);\n"
        "        int8[] out #= h.digest();\n"
        "        return #out;\n"
        "    }\n}\n";
    EXPECT_EQ(hex(runBytes(src)), "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(Pbkdf2Tests, rfc7914VectorOneIteration) {
    EXPECT_EQ(hex(runBytes(pbkdf2Src("passwd", "salt", 1, 64))),
        "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
        "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
}

TEST(Pbkdf2Tests, rfc7914Vector80000Iterations) {
    EXPECT_EQ(hex(runBytes(pbkdf2Src("Password", "NaCl", 80000, 64))),
        "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
        "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d");
}

TEST(Pbkdf2Tests, saltChangesEveryByte) {
    auto a = runBytes(pbkdf2Src("passwd", "salt", 2, 32));
    auto b = runBytes(pbkdf2Src("passwd", "salt2", 2, 32));
    ASSERT_EQ(a.size(), 32u);
    ASSERT_EQ(b.size(), 32u);
    int same = 0;
    for (size_t i = 0; i < 32; i++) if (a[i] == b[i]) same++;
    EXPECT_LE(same, 4);
}

TEST(SecureRandomTests, drawsDifferAndHonorLength) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.SecureRandom;\n"
        "public final class D {\n"
        "    public static #int8[] run() {\n"
        "        int8[] a #= SecureRandom.bytes(32L);\n"
        "        int8[] b #= SecureRandom.bytes(32L);\n"
        "        int8[] out = heap int8[64];\n"
        "        int64 i = 0;\n"
        "        while (i < 32L) { out[i] = a[i]; out[32L + i] = b[i]; i = i + 1; }\n"
        "        return #out;\n"
        "    }\n}\n";
    auto v = runBytes(src);
    ASSERT_EQ(v.size(), 64u);
    std::vector<uint8_t> a(v.begin(), v.begin() + 32), b(v.begin() + 32, v.end());
    EXPECT_NE(a, b);
    EXPECT_NE(a, repeated(0, 32));
}

TEST(SecureRandomTests, zeroLengthIsANoOp) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.SecureRandom;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] z #= SecureRandom.bytes(0L);\n"
        "        int8[] buf = heap int8[4];\n"
        "        buf[0] = (int8) 7; buf[1] = (int8) 7; buf[2] = (int8) 7; buf[3] = (int8) 7;\n"
        "        SecureRandom.fill(buf, 0L);\n"
        "        if (buf[0] != (int8) 7 || buf[3] != (int8) 7) { return 2; }\n"
        "        return (int32) z.count();\n"
        "    }\n}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 0);
}

TEST(SecureRandomTests, fillsThroughTheNativeEntropyPath) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.SecureRandom;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] a #= SecureRandom.bytes(8L);\n"
        "        return (int32) a.count();\n"
        "    }\n}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    std::string body = functionBody(jit->getModuleIr(), "cajeta.hash.SecureRandom::fill");
    ASSERT_FALSE(body.empty());
    EXPECT_NE(body.find("@__cajeta_secure_random_fill"), std::string::npos);
    EXPECT_EQ(body.find("cajeta.math"), std::string::npos);
}

TEST(ConstantTimeEqualsTests, equalUnequalAndLengths) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Hash;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + bytesDecl("a", ascii("secret-token")) + bytesDecl("b", ascii("secret-token")) + bytesDecl("c", ascii("secret-tokeN")) +
        "        int32 r = 0;\n"
        "        if (Hash.constantTimeEquals(a, 12L, b, 12L)) { r = r + 1; }\n"
        "        if (Hash.constantTimeEquals(a, 12L, c, 12L)) { r = r + 10; }\n"
        "        if (Hash.constantTimeEquals(a, 12L, b, 11L)) { r = r + 100; }\n"
        "        if (Hash.constantTimeEquals(a, 0L, b, 0L)) { r = r + 1000; }\n"
        "        return r;\n"
        "    }\n}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1001);
}

TEST(ConstantTimeEqualsTests, noBranchDependsOnTheBytesAndTheControlDoes) {
    std::string src =
        "package test;\n"
        "import cajeta.hash.Hash;\n"
        "public final class D {\n"
        "    public static boolean earlyExit(int8[] a, int64 aLen, int8[] b, int64 bLen) {\n"
        "        if (aLen != bLen) { return false; }\n"
        "        int64 i = 0;\n"
        "        while (i < aLen) { if (a[i] != b[i]) { return false; } i = i + 1; }\n"
        "        return true;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int8[] a = heap int8[4];\n"
        "        int8[] b = heap int8[4];\n"
        "        boolean x = Hash.constantTimeEquals(a, 4L, b, 4L);\n"
        "        boolean y = D.earlyExit(a, 4L, b, 4L);\n"
        "        return (x && y) ? 1 : 0;\n"
        "    }\n}\n";
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    const std::string& ir = jit->getModuleIr();
    std::string control = functionBody(ir, "test.D::earlyExit");
    ASSERT_FALSE(control.empty());
    EXPECT_TRUE(branchesOnLoadedData(control)) << "the control must be caught";
    std::string subject = functionBody(ir, "cajeta.hash.Hash::constantTimeEquals");
    ASSERT_FALSE(subject.empty());
    EXPECT_FALSE(branchesOnLoadedData(subject));
}
