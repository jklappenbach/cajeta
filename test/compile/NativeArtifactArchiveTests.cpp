// Native-deps unit 3 — the .cja `native/` tree (per-platform artifacts +
// headers + embedded metadata). See native-deps-plan.md unit 3, spec §3.

#include <gtest/gtest.h>
#include "cajeta/compile/CajetaArchive.h"

#include <filesystem>
#include <string>
#include <vector>

using cajeta::CajetaArchive;

namespace {
std::vector<uint8_t> bytesOf(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
std::string dataStr(const cajeta::CajetaArchiveEntry* e) {
    return std::string(e->data.begin(), e->data.end());
}
std::string tmpCja(const std::string& tag) {
    return (std::filesystem::temp_directory_path() /
            ("nd-native-archive-" + tag + ".cja")).string();
}
} // namespace

// 3.1.1 — artifacts + headers + metadata round-trip byte-exact.
TEST(NativeArtifactArchiveTests, roundTripsArtifactsHeadersAndMeta) {
    CajetaArchive arc("dev.cajeta.codec", "0.1.0", CajetaArchive::Kind::Cja);
    arc.addNativeArtifact("linux-x64", "libzstd.a", bytesOf("ZSTD-X64-BINARY"));
    arc.addNativeHeader("zstd.h", bytesOf("#define ZSTD 1\n"));
    arc.setNativeLibrariesMeta(bytesOf("{\"zstd\":{\"version\":\"1.5\"}}"));
    std::string path = tmpCja("rt");
    arc.writeTo(path);

    auto back = CajetaArchive::readFrom(path);
    auto plats = back.nativePlatforms();
    ASSERT_EQ(plats.size(), 1u);
    EXPECT_EQ(*plats.begin(), "linux-x64");

    auto arts = back.nativeArtifactsFor("linux-x64");
    ASSERT_EQ(arts.size(), 1u);
    EXPECT_EQ(arts[0]->name, "native/linux-x64/libzstd.a");
    EXPECT_EQ(dataStr(arts[0]), "ZSTD-X64-BINARY");

    auto hdrs = back.nativeHeaders();
    ASSERT_EQ(hdrs.size(), 1u);
    EXPECT_EQ(hdrs[0]->name, "native/include/zstd.h");
    EXPECT_EQ(dataStr(hdrs[0]), "#define ZSTD 1\n");

    const auto* meta = back.nativeLibrariesMeta();
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(dataStr(meta), "{\"zstd\":{\"version\":\"1.5\"}}");
    std::filesystem::remove(path);
}

// 3.1.2 — multi-platform: select by platform; unknown platform → empty.
TEST(NativeArtifactArchiveTests, selectsByPlatformEmptyForUnknown) {
    CajetaArchive arc("x", "1", CajetaArchive::Kind::Cja);
    arc.addNativeArtifact("linux-x64", "libz.a", bytesOf("X64"));
    arc.addNativeArtifact("linux-arm64", "libz.a", bytesOf("ARM64"));
    std::string path = tmpCja("multi");
    arc.writeTo(path);

    auto back = CajetaArchive::readFrom(path);
    EXPECT_EQ(back.nativePlatforms().size(), 2u);
    auto x = back.nativeArtifactsFor("linux-x64");
    ASSERT_EQ(x.size(), 1u);
    EXPECT_EQ(dataStr(x[0]), "X64");
    auto a = back.nativeArtifactsFor("linux-arm64");
    ASSERT_EQ(a.size(), 1u);
    EXPECT_EQ(dataStr(a[0]), "ARM64");
    EXPECT_TRUE(back.nativeArtifactsFor("windows-x64").empty());  // unknown
    std::filesystem::remove(path);
}

// 3.1.3 — a slim .cja (no native tree) reads cleanly: empty everywhere.
TEST(NativeArtifactArchiveTests, slimArchiveHasNoNativeTree) {
    CajetaArchive arc("x", "1", CajetaArchive::Kind::Cja);
    std::string path = tmpCja("slim");
    arc.writeTo(path);
    auto back = CajetaArchive::readFrom(path);
    EXPECT_TRUE(back.nativePlatforms().empty());
    EXPECT_TRUE(back.nativeArtifactsFor("linux-x64").empty());
    EXPECT_TRUE(back.nativeHeaders().empty());
    EXPECT_EQ(back.nativeLibrariesMeta(), nullptr);
    std::filesystem::remove(path);
}
