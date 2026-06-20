// Native-deps unit 9 — provisioning: fetch (→ cache, sha256-verified), vendor,
// and offline cache resolution. See native-deps-plan.md unit 9, spec §6.

#include "cajeta/buildtool/NativeProvision.h"
#include "cajeta/buildtool/ArtifactCache.h"

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
} // namespace

// 9.1.1 — fetch verifies sha256 and lands the artifact in the versioned cache.
TEST(NativeProvisionTests, fetchVerifiesAndCaches) {
    auto root = fs::temp_directory_path() / "nd-prov-fetch";
    fs::remove_all(root);
    auto src = writeFile(root / "src" / "libzstd.a", "ZSTD-BYTES");
    std::string sha = ArtifactCache::sha256OfFile(src);     // correct digest

    auto cacheRoot = (root / "cache").string();
    auto r = fetchNativeToCache(cacheRoot, "zstd", "1.5.2", "linux-x64", src, sha);
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    EXPECT_EQ(*r, cacheRoot + "/zstd/1.5.2/linux-x64/libzstd.a");
    std::ifstream f(*r, std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(f)), {});
    EXPECT_EQ(got, "ZSTD-BYTES");
    fs::remove_all(root);
}

// 9.1.1 — a sha256 mismatch fails loud and does NOT cache.
TEST(NativeProvisionTests, fetchShaMismatchFailsLoud) {
    auto root = fs::temp_directory_path() / "nd-prov-mismatch";
    fs::remove_all(root);
    auto src = writeFile(root / "src" / "libzstd.a", "ZSTD-BYTES");
    auto cacheRoot = (root / "cache").string();
    auto r = fetchNativeToCache(cacheRoot, "zstd", "1.5.2", "linux-x64", src,
                                "sha256:deadbeef");
    ASSERT_FALSE((bool) r);
    EXPECT_NE(errText(r.takeError()).find("sha256 mismatch"), std::string::npos);
    EXPECT_FALSE(fs::exists(cacheRoot + "/zstd/1.5.2/linux-x64/libzstd.a"));
    fs::remove_all(root);
}

// 9.1.2 — vendor copies into the project native/<platform>/ dir.
TEST(NativeProvisionTests, vendorMaterializesIntoProjectDir) {
    auto root = fs::temp_directory_path() / "nd-prov-vendor";
    fs::remove_all(root);
    auto src = writeFile(root / "src" / "libzstd.a", "Z");
    auto proj = (root / "proj" / "native").string();
    auto r = vendorNativeArtifact(proj, "linux-arm64", src);
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    EXPECT_EQ(*r, proj + "/linux-arm64/libzstd.a");
    EXPECT_TRUE(fs::exists(*r));
    fs::remove_all(root);
}

// 9.1.3 — after fetch, the cache provider resolves the lib fully offline.
TEST(NativeProvisionTests, cacheProviderResolvesOffline) {
    auto root = fs::temp_directory_path() / "nd-prov-offline";
    fs::remove_all(root);
    auto src = writeFile(root / "src" / "libzstd.a", "Z");
    auto cacheRoot = (root / "cache").string();
    auto fr = fetchNativeToCache(cacheRoot, "zstd", "1.5.2", "linux-x64", src, "");
    ASSERT_TRUE((bool) fr) << errText(fr.takeError());

    // Direct cache hit.
    auto hit = findInNativeCache(cacheRoot, "zstd", "1.5.2", "linux-x64");
    ASSERT_TRUE(hit.has_value());

    // Through the unit-5 resolver via the cache provider.
    NativeRequirementSet reqs;
    reqs.required.insert("zstd");
    reqs.versionConstraints["zstd"] = {"1.5.2"};
    auto res = resolveNatives(reqs, {}, "linux-x64",
                              {cacheNativeProvider(cacheRoot)});
    ASSERT_TRUE(res.resolved.count("zstd"));
    EXPECT_EQ(res.resolved.at("zstd").via, "cache");
    EXPECT_EQ(res.resolved.at("zstd").artifactPath, *hit);
    fs::remove_all(root);
}

// A missing source fails loud (no partial cache).
TEST(NativeProvisionTests, fetchMissingSourceFailsLoud) {
    auto r = fetchNativeToCache("/tmp/nd-cache-x", "zstd", "1.5.2", "linux-x64",
                                "/no/such/source.a", "");
    ASSERT_FALSE((bool) r);
    EXPECT_NE(errText(r.takeError()).find("source not found"), std::string::npos);
}
