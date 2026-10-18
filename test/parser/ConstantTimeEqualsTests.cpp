// The constant-time byte compare, Hash.constantTimeEquals: it stays in the
// stdlib when the cryptographic types move to dev.cajeta.crypto (cajeta-crypto
// spec 6.3), so its cases stay here; CryptoPrimitivesTests' HMAC-SHA256,
// PBKDF2 and SecureRandom cases moved with those types.

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

std::vector<uint8_t> ascii(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

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

}  // namespace

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
