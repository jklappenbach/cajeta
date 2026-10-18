//
// cajeta-crypto spec 6.3, plan 7.1.1: the nine cryptographic cajeta.hash types
// and cajeta.io.net.tls left the stdlib for the dev.cajeta.crypto library. Code
// that still imports one and uses it fails to compile with the type's name in
// the error, and the release notes give the new import. An import that is
// never used compiles, as an unused import of any missing type does.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compiles `src` and returns the error's id and message, or "" when it compiled.
std::string compileError(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        return e.getErrorId() + ": " + e.getMessage();
    }
    return "";
}

}  // namespace

TEST(RemovedCryptoTypesTests, aRemovedDigestNamesTheType) {
    std::string err = compileError(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.hash.Sha256;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        String h #= Sha256.hashStringHex(\"abc\");\n"
        "        return h.byteLength();\n"
        "    }\n"
        "}\n");
    EXPECT_NE(err.find("CAJETA_ERROR_UNRESOLVED_TYPE"), std::string::npos) << err;
    EXPECT_NE(err.find("Sha256"), std::string::npos) << err;
}

TEST(RemovedCryptoTypesTests, aRemovedRandomSourceNamesTheType) {
    std::string err = compileError(
        "package test;\n"
        "import cajeta.hash.SecureRandom;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b #= SecureRandom.bytes(16L);\n"
        "        return (int32) b.count();\n"
        "    }\n"
        "}\n");
    EXPECT_NE(err.find("CAJETA_ERROR_UNRESOLVED_TYPE"), std::string::npos) << err;
    EXPECT_NE(err.find("SecureRandom"), std::string::npos) << err;
}

TEST(RemovedCryptoTypesTests, aRemovedTlsTypeNamesTheType) {
    std::string err = compileError(
        "package test;\n"
        "import cajeta.io.net.tls.TlsConnection;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        TlsConnection c #= TlsConnection.client();\n"
        "        return c.pending();\n"
        "    }\n"
        "}\n");
    EXPECT_NE(err.find("CAJETA_ERROR_UNRESOLVED_TYPE"), std::string::npos) << err;
    EXPECT_NE(err.find("TlsConnection"), std::string::npos) << err;
}

// The stdlib's non-cryptographic hashing stays: the same file compiles with
// XXHash3, so the three cases above fail on the removal and not on the harness.
TEST(RemovedCryptoTypesTests, theStdlibHashesThatStayStillCompile) {
    std::string err = compileError(
        "package test;\n"
        "import cajeta.hash.XXHash3;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return XXHash3.hashString(\"abc\") == 0L ? 1 : 0;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(err, "");
}
