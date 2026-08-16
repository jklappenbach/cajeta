// Per-action regression tests for the Phase 4 native catalog.
// See src/cajeta/buildtool/actions/ for the implementations.

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Manifest.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::ActionRegistry;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::TaskContext;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    cajeta::buildtool::ResolvedProperties makeProps() {
        // Minimal manifest just to get a ResolvedProperties.
        auto m = loadManifestString(
            R"({"details":{"name":"a.b","version":"0.1"}})");
        if (!m) {
            ADD_FAILURE() << errorText(m.takeError());
            return {};
        }
        auto r = resolveProperties(*m);
        if (!r) {
            ADD_FAILURE() << errorText(r.takeError());
            return {};
        }
        return std::move(*r);
    }

    std::filesystem::path tempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-action-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    void writeFile(const std::filesystem::path& p, const std::string& s) {
        std::ofstream o(p, std::ios::binary);
        o << s;
    }

    std::string readFileStr(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        return ss.str();
    }

} // namespace

// ─── mkdir / copy / delete ────────────────────────────────────────────

TEST(ActionCatalogTests, mkdirCreatesDirectory) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto dir = tempDir("mkdir") / "nested" / "subdir";
    llvm::json::Object params;
    params["path"] = dir.string();
    auto r = registry.get("mkdir")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_TRUE(std::filesystem::exists(dir));
    std::filesystem::remove_all(dir.parent_path().parent_path());
}

TEST(ActionCatalogTests, copyCopiesFile) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("copy");
    auto src = d / "src.txt";
    auto dst = d / "dst.txt";
    writeFile(src, "hello");

    llvm::json::Object params;
    params["from"] = src.string();
    params["to"]   = dst.string();
    auto r = registry.get("copy")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_TRUE(std::filesystem::exists(dst));
    EXPECT_EQ(readFileStr(dst), "hello");
    std::filesystem::remove_all(d);
}

TEST(ActionCatalogTests, copyErrorsOnMissingSource) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("copy-missing");
    llvm::json::Object params;
    params["from"] = (d / "no-such-file").string();
    params["to"]   = (d / "out").string();
    auto r = registry.get("copy")->run(params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("source does not exist"), std::string::npos);
    std::filesystem::remove_all(d);
}

TEST(ActionCatalogTests, deleteRemovesFile) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("delete");
    auto victim = d / "victim.txt";
    writeFile(victim, "x");

    llvm::json::Object params;
    params["paths"] = victim.string();
    auto r = registry.get("delete")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_FALSE(std::filesystem::exists(victim));
    std::filesystem::remove_all(d);
}


TEST(ActionCatalogTests, deleteStrictErrorsOnMissing) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("delete-strict");
    llvm::json::Object params;
    params["paths"] = (d / "ghost").string();
    params["if-exists"] = false;
    auto r = registry.get("delete")->run(params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("if-exists=false"), std::string::npos);
    std::filesystem::remove_all(d);
}

// ─── version ──────────────────────────────────────────────────────────

TEST(ActionCatalogTests, versionBumpPatchMutatesManifest) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("version");
    auto manifest = d / "cajeta.json";
    writeFile(manifest, R"({
    "details": { "name": "a.b", "version": "1.2.3" }
})");

    llvm::json::Object params;
    params["bump"] = "patch";
    params["write-to"] = manifest.string();
    auto r = registry.get("version")->run(params, ctx);
    ASSERT_TRUE((bool)r) << errorText(r.takeError());
    EXPECT_EQ(r->outputs.at("previous"), "1.2.3");
    EXPECT_EQ(r->outputs.at("version"), "1.2.4");
    EXPECT_NE(readFileStr(manifest).find("\"version\": \"1.2.4\""),
              std::string::npos);
    std::filesystem::remove_all(d);
}

TEST(ActionCatalogTests, versionBumpMinorResetsPatch) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("version-minor");
    auto manifest = d / "cajeta.json";
    writeFile(manifest, R"({"details":{"name":"a.b","version":"1.2.7"}})");

    llvm::json::Object params;
    params["bump"] = "minor";
    params["write-to"] = manifest.string();
    auto r = registry.get("version")->run(params, ctx);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->outputs.at("version"), "1.3.0");
    std::filesystem::remove_all(d);
}

TEST(ActionCatalogTests, versionSetReplacesExactly) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("version-set");
    auto manifest = d / "cajeta.json";
    writeFile(manifest, R"({"details":{"name":"a.b","version":"0.1.0"}})");

    llvm::json::Object params;
    params["set"] = "2.0.0-rc1";
    params["write-to"] = manifest.string();
    auto r = registry.get("version")->run(params, ctx);
    ASSERT_TRUE((bool)r);
    EXPECT_EQ(r->outputs.at("version"), "2.0.0-rc1");
    std::filesystem::remove_all(d);
}


TEST(ActionCatalogTests, versionErrorsWhenBothBumpAndSet) {
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("version-both");
    auto manifest = d / "cajeta.json";
    writeFile(manifest, R"({"details":{"name":"a.b","version":"0.1.0"}})");

    llvm::json::Object params;
    params["bump"] = "patch";
    params["set"]  = "1.0.0";
    params["write-to"] = manifest.string();
    auto r = registry.get("version")->run(params, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("not both"), std::string::npos);
    std::filesystem::remove_all(d);
}

// ─── sign + verify-sig ────────────────────────────────────────────────
//
// These require openssl on PATH to generate a test keypair, which is
// always available in the cajeta build environment (libcrypto is a
// build-time dep). We generate a fresh ed25519 keypair per test, sign a
// payload, then verify it (and verify a tampered payload fails).

namespace {

    bool haveOpenSslOnPath() {
        return std::system("which openssl > /dev/null 2>&1") == 0;
    }

    struct KeyPair {
        std::filesystem::path priv;
        std::filesystem::path pub;
    };

    KeyPair generateEd25519(const std::filesystem::path& dir) {
        KeyPair k;
        k.priv = dir / "priv.pem";
        k.pub  = dir / "pub.pem";
        std::string cmd = "openssl genpkey -algorithm ed25519 -out '" +
                          k.priv.string() + "' > /dev/null 2>&1";
        (void)std::system(cmd.c_str());
        cmd = "openssl pkey -in '" + k.priv.string() + "' -pubout -out '" +
              k.pub.string() + "' > /dev/null 2>&1";
        (void)std::system(cmd.c_str());
        return k;
    }

} // namespace


TEST(ActionCatalogTests, verifySigFailsOnTamperedPayload) {
    if (!haveOpenSslOnPath()) {
        GTEST_SKIP() << "openssl not on PATH";
    }
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("sign-tamper");
    auto keys = generateEd25519(d);
    auto payload = d / "payload.bin";
    writeFile(payload, "original payload");

    llvm::json::Object signParams;
    signParams["input"]    = payload.string();
    signParams["key-path"] = keys.priv.string();
    signParams["key-id"]   = "test-key";
    auto signed_ = registry.get("sign")->run(signParams, ctx);
    ASSERT_TRUE((bool)signed_);

    // Tamper.
    writeFile(payload, "TAMPERED payload");

    llvm::json::Object verifyParams;
    verifyParams["input"]       = payload.string();
    verifyParams["pubkey-path"] = keys.pub.string();
    auto verified = registry.get("verify-sig")->run(verifyParams, ctx);
    ASSERT_TRUE((bool)verified) << errorText(verified.takeError());
    EXPECT_EQ(verified->outputs.at("valid"), "false");

    std::filesystem::remove_all(d);
}

TEST(ActionCatalogTests, signRejectsNonEd25519Key) {
    if (!haveOpenSslOnPath()) {
        GTEST_SKIP() << "openssl not on PATH";
    }
    auto props = makeProps();
    ActionRegistry registry;
    TaskContext ctx(props);

    auto d = tempDir("sign-bad-key");
    auto key = d / "rsa.pem";
    (void)std::system(("openssl genpkey -algorithm RSA -out '" +
                       key.string() +
                       "' -pkeyopt rsa_keygen_bits:2048 > /dev/null 2>&1")
                          .c_str());
    auto payload = d / "payload.bin";
    writeFile(payload, "x");

    llvm::json::Object signParams;
    signParams["input"]    = payload.string();
    signParams["key-path"] = key.string();
    signParams["key-id"]   = "rsa-key";
    auto r = registry.get("sign")->run(signParams, ctx);
    ASSERT_FALSE((bool)r);
    auto msg = errorText(r.takeError());
    EXPECT_NE(msg.find("not ed25519"), std::string::npos);
    std::filesystem::remove_all(d);
}
