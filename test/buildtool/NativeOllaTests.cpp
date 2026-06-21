// Native-deps unit 10 — Olla native mirror: redistributable fetch + verify,
// embargoed metadata-only guard. See native-deps-plan.md unit 10, spec §8.

#include "cajeta/buildtool/NativeProvision.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace cajeta::buildtool;
namespace fs = std::filesystem;

namespace {
std::string errText(llvm::Error&& e) {
    std::string s; llvm::raw_string_ostream os(s); os << e;
    consumeError(std::move(e)); return s;
}
std::string writeFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << content;
    return p.string();
}
NativeLibrary lib(const std::string& id, bool redistributable) {
    NativeLibrary l;
    l.id = id; l.version = "1.5.2"; l.license = "BSD-3-Clause";
    l.redistributable = redistributable;
    return l;
}
} // namespace

// Publish/fetch guard: only redistributable binaries may be mirrored.
TEST(NativeOllaTests, mirrorGuardGatesOnRedistributable) {
    EXPECT_TRUE(mayMirrorNativeBinary(lib("zstd", true)));
    EXPECT_FALSE(mayMirrorNativeBinary(lib("vendorx", false)));
}

// 10.1.1 — redistributable artifact fetched from the (stub) mirror into cache.
TEST(NativeOllaTests, fetchesRedistributableFromMirror) {
    auto root = fs::temp_directory_path() / "nd-olla-redis";
    fs::remove_all(root);
    auto mirror = (root / "mirror").string();
    writeFile(fs::path(mirror) / "zstd" / "1.5.2" / "linux-x64" / "libzstd.a",
              "ZSTD");
    auto cacheRoot = (root / "cache").string();

    auto r = fetchFromOllaMirror(lib("zstd", true), "1.5.2", "linux-x64",
                                 mirror, cacheRoot);
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    EXPECT_EQ(*r, cacheRoot + "/zstd/1.5.2/linux-x64/libzstd.a");
    EXPECT_TRUE(fs::exists(*r));
    fs::remove_all(root);
}

// 10.1.2 — an embargoed lib is refused: Olla serves metadata only, directing to
// local provisioning (with its acquire note).
TEST(NativeOllaTests, embargoedIsMetadataOnly) {
    auto root = fs::temp_directory_path() / "nd-olla-emb";
    fs::remove_all(root);
    auto mirror = (root / "mirror").string();
    // Even if a binary somehow sat on the mirror, the guard refuses it.
    writeFile(fs::path(mirror) / "vendorx" / "2.0" / "linux-x64" / "libvendorx.a",
              "X");
    auto cacheRoot = (root / "cache").string();

    auto el = lib("vendorx", false);
    el.acquire = "download from vendor.example/dl";
    auto r = fetchFromOllaMirror(el, "2.0", "linux-x64", mirror, cacheRoot);
    ASSERT_FALSE((bool) r);
    std::string msg = errText(r.takeError());
    EXPECT_NE(msg.find("embargoed"), std::string::npos);
    EXPECT_NE(msg.find("metadata only"), std::string::npos);
    EXPECT_NE(msg.find("vendor.example/dl"), std::string::npos);  // acquire note
    EXPECT_FALSE(fs::exists(cacheRoot + "/vendorx/2.0/linux-x64/libvendorx.a"));
    fs::remove_all(root);
}

// A redistributable lib missing from the mirror fails loud.
TEST(NativeOllaTests, missingOnMirrorFailsLoud) {
    auto root = fs::temp_directory_path() / "nd-olla-missing";
    fs::remove_all(root);
    auto r = fetchFromOllaMirror(lib("zstd", true), "1.5.2", "linux-x64",
                                 (root / "mirror").string(),
                                 (root / "cache").string());
    ASSERT_FALSE((bool) r);
    EXPECT_NE(errText(r.takeError()).find("no artifact"), std::string::npos);
    fs::remove_all(root);
}
