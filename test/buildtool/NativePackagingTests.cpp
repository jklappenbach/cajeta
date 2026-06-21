// Native-deps unit 6 — default packaging step (bake into .cja/ubercja).
// See native-deps-plan.md unit 6, spec §3.3.

#include "cajeta/buildtool/NativeResolver.h"
#include "cajeta/compile/CajetaArchive.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <filesystem>
#include <fstream>
#include <string>

using namespace cajeta::buildtool;
using cajeta::CajetaArchive;

namespace {
std::string errText(llvm::Error&& e) {
    std::string s; llvm::raw_string_ostream os(s); os << e;
    consumeError(std::move(e)); return s;
}
std::string dataStr(const cajeta::CajetaArchiveEntry* e) {
    return std::string(e->data.begin(), e->data.end());
}
std::string writeTempFile(const std::string& name, const std::string& content) {
    auto p = std::filesystem::temp_directory_path() / ("nd-pkg-" + name);
    std::ofstream f(p, std::ios::binary);
    f << content;
    f.close();
    return p.string();
}
NativeRequirementSet reqZstd(bool redistributable) {
    NativeRequirementSet r;
    r.required.insert("zstd");
    NativeLibrary lib;
    lib.id = "zstd"; lib.version = "1.5.2"; lib.license = "BSD-3-Clause";
    lib.link = "static"; lib.redistributable = redistributable;
    r.libraries["zstd"] = lib;
    return r;
}
ResolvedNative resolved(const std::string& platform, const std::string& path) {
    return ResolvedNative{"zstd", "1.5.2", platform, path, "static", "cache"};
}
} // namespace

// 6.1.1 — bake a resolved artifact + embed metadata that round-trips.
TEST(NativePackagingTests, bakesArtifactAndMetadata) {
    std::string art = writeTempFile("libzstd-x64.a", "ZSTD-X64-BYTES");
    auto reqs = reqZstd(true);
    NativeResolution res; res.resolved["zstd"] = resolved("linux-x64", art);
    std::map<std::string, NativeResolution> per{{"linux-x64", res}};

    CajetaArchive arc("app", "1.0", CajetaArchive::Kind::Cja);
    auto r = bakeNativeArtifacts(arc, reqs, per, /*slim=*/false);
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    EXPECT_TRUE(r->missing.empty());

    auto arts = arc.nativeArtifactsFor("linux-x64");
    ASSERT_EQ(arts.size(), 1u);
    // Entry name = native/<platform>/<basename-of-resolved-path>.
    EXPECT_EQ(arts[0]->name, "native/linux-x64/nd-pkg-libzstd-x64.a");
    EXPECT_EQ(dataStr(arts[0]), "ZSTD-X64-BYTES");

    const auto* meta = arc.nativeLibrariesMeta();
    ASSERT_NE(meta, nullptr);
    auto parsed = parseNativeMeta(
        llvm::StringRef(reinterpret_cast<const char*>(meta->data.data()),
                        meta->data.size()), "meta");
    ASSERT_TRUE((bool) parsed) << errText(parsed.takeError());
    EXPECT_TRUE(parsed->requiredLibs.count("zstd"));
    ASSERT_TRUE(parsed->libraries.count("zstd"));
    EXPECT_EQ(parsed->libraries.at("zstd").version, "1.5.2");
    std::filesystem::remove(art);
}

// 6.1.2 — an embargoed (redistributable:false) but provisioned artifact is
// baked into the app archive (the flag only gates Olla/public bundling).
TEST(NativePackagingTests, embargoedProvisionedArtifactIsBaked) {
    std::string art = writeTempFile("libembargo.a", "EMBARGO");
    auto reqs = reqZstd(/*redistributable=*/false);
    NativeResolution res; res.resolved["zstd"] = resolved("linux-x64", art);
    std::map<std::string, NativeResolution> per{{"linux-x64", res}};

    CajetaArchive arc("app", "1.0", CajetaArchive::Kind::Uber);
    auto r = bakeNativeArtifacts(arc, reqs, per, /*slim=*/false);
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    EXPECT_EQ(arc.nativeArtifactsFor("linux-x64").size(), 1u);  // baked anyway
    EXPECT_TRUE(r->missing.empty());
    std::filesystem::remove(art);
}

// 6.1.3 — slim opts out: no native/ tree; an unresolved required lib is
// reported (not silently dropped); a resolved-but-not-baked lib is not.
TEST(NativePackagingTests, slimSkipsBakeAndReportsUnresolved) {
    // zstd resolved but slim → not baked, not missing.
    std::string art = writeTempFile("libzstd2.a", "X");
    auto reqs = reqZstd(true);
    reqs.required.insert("lz4");   // required but never resolved → missing
    NativeResolution res; res.resolved["zstd"] = resolved("linux-x64", art);
    std::map<std::string, NativeResolution> per{{"linux-x64", res}};

    CajetaArchive arc("app", "1.0", CajetaArchive::Kind::Cja);
    auto r = bakeNativeArtifacts(arc, reqs, per, /*slim=*/true);
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    EXPECT_TRUE(arc.nativePlatforms().empty());          // no native/ tree
    EXPECT_EQ(arc.nativeLibrariesMeta(), nullptr);
    ASSERT_EQ(r->missing.size(), 1u);                    // lz4 only
    EXPECT_EQ(r->missing[0], "lz4");
    std::filesystem::remove(art);
}

// 6.1.4 — ubercja bundles every targeted platform.
TEST(NativePackagingTests, bakesAllTargetedPlatforms) {
    std::string a64 = writeTempFile("libzstd-a64.a", "ARM64");
    std::string x64 = writeTempFile("libzstd-x64b.a", "X64");
    auto reqs = reqZstd(true);
    NativeResolution rx; rx.resolved["zstd"] = resolved("linux-x64", x64);
    NativeResolution ra; ra.resolved["zstd"] = resolved("linux-arm64", a64);
    std::map<std::string, NativeResolution> per{
        {"linux-x64", rx}, {"linux-arm64", ra}};

    CajetaArchive arc("app", "1.0", CajetaArchive::Kind::Uber);
    auto r = bakeNativeArtifacts(arc, reqs, per, /*slim=*/false);
    ASSERT_TRUE((bool) r) << errText(r.takeError());
    EXPECT_EQ(arc.nativePlatforms().size(), 2u);
    EXPECT_EQ(arc.nativeArtifactsFor("linux-x64").size(), 1u);
    EXPECT_EQ(arc.nativeArtifactsFor("linux-arm64").size(), 1u);
    std::filesystem::remove(a64);
    std::filesystem::remove(x64);
}

// A missing artifact file fails loud.
TEST(NativePackagingTests, missingArtifactFileFailsLoud) {
    auto reqs = reqZstd(true);
    NativeResolution res;
    res.resolved["zstd"] = resolved("linux-x64", "/no/such/path/libzstd.a");
    std::map<std::string, NativeResolution> per{{"linux-x64", res}};
    CajetaArchive arc("app", "1.0", CajetaArchive::Kind::Cja);
    auto r = bakeNativeArtifacts(arc, reqs, per, /*slim=*/false);
    ASSERT_FALSE((bool) r);
    EXPECT_NE(errText(r.takeError()).find("cannot read native artifact"),
              std::string::npos);
}
