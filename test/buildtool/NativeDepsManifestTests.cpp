// Native-deps unit 1 — settings.native-libraries / settings.native-overrides
// manifest parsing. See agents/cajeta/buildtool/native-deps-plan.md unit 1 and
// docs/specification/buildtool/native-deps-spec.md §2.

#include "cajeta/buildtool/Manifest.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>

using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseNativeLibraries;
using cajeta::buildtool::parseNativeOverrides;

namespace {
std::string errorText(llvm::Error&& err) {
    std::string out;
    llvm::raw_string_ostream os(out);
    os << err;
    consumeError(std::move(err));
    return out;
}
} // namespace

// 1.1.1 — full block round-trips every field.
TEST(NativeDepsManifestTests, parsesFullNativeLibrariesBlock) {
    std::string src = R"({
      "details":{"name":"dev.cajeta.codec","version":"0.1.0"},
      "settings":{"native-libraries":{
        "zstd":{
          "version":"1.5.*","license":"BSD-3-Clause","redistributable":true,
          "link":"static","platforms":["linux-x64","linux-arm64"],
          "artifacts":{
            "linux-x64":{"url":"https://x/zstd-x64.a","sha256":"aa"},
            "linux-arm64":{"url":"https://x/zstd-arm.a","sha256":"bb"}
          }
        }
      }}
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m) << errorText(m.takeError());
    auto libs = parseNativeLibraries(*m);
    ASSERT_TRUE((bool) libs) << errorText(libs.takeError());
    ASSERT_EQ(libs->size(), 1u);
    const auto& z = libs->at("zstd");
    EXPECT_EQ(z.id, "zstd");
    EXPECT_EQ(z.version, "1.5.*");
    EXPECT_EQ(z.license, "BSD-3-Clause");
    EXPECT_TRUE(z.redistributable);
    EXPECT_EQ(z.link, "static");
    ASSERT_EQ(z.platforms.size(), 2u);
    EXPECT_EQ(z.platforms[0], "linux-x64");
    ASSERT_EQ(z.artifacts.size(), 2u);
    EXPECT_EQ(z.artifacts.at("linux-x64").sha256.value_or(""), "aa");
    EXPECT_EQ(z.artifacts.at("linux-arm64").url.value_or(""),
              "https://x/zstd-arm.a");
}

// 1.1.1 — defaults: link=static, redistributable=false; embargoed acquire.
TEST(NativeDepsManifestTests, defaultsAndEmbargoedAcquire) {
    std::string src = R"({
      "details":{"name":"a.b","version":"0.1"},
      "settings":{"native-libraries":{
        "embargoedlib":{
          "version":"2.0","license":"LicenseRef-Vendor-EULA",
          "acquire":"download from vendor.example/dl and place in native/"
        }
      }}
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m) << errorText(m.takeError());
    auto libs = parseNativeLibraries(*m);
    ASSERT_TRUE((bool) libs) << errorText(libs.takeError());
    const auto& e = libs->at("embargoedlib");
    EXPECT_EQ(e.link, "static");          // default
    EXPECT_FALSE(e.redistributable);      // default
    EXPECT_TRUE(e.acquire.has_value());
}

// 1.1.2 — missing required version fails loud.
TEST(NativeDepsManifestTests, rejectsMissingVersion) {
    std::string src = R"({
      "details":{"name":"a.b","version":"0.1"},
      "settings":{"native-libraries":{"zstd":{"license":"BSD-3-Clause"}}}
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m);
    auto libs = parseNativeLibraries(*m);
    ASSERT_FALSE((bool) libs);
    EXPECT_NE(errorText(libs.takeError()).find("missing required 'version'"),
              std::string::npos);
}

// 1.1.2 — missing required license fails loud.
TEST(NativeDepsManifestTests, rejectsMissingLicense) {
    std::string src = R"({
      "details":{"name":"a.b","version":"0.1"},
      "settings":{"native-libraries":{"zstd":{"version":"1.5"}}}
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m);
    auto libs = parseNativeLibraries(*m);
    ASSERT_FALSE((bool) libs);
    EXPECT_NE(errorText(libs.takeError()).find("missing required 'license'"),
              std::string::npos);
}

// 1.1.2 — unknown link mode fails loud.
TEST(NativeDepsManifestTests, rejectsUnknownLinkMode) {
    std::string src = R"({
      "details":{"name":"a.b","version":"0.1"},
      "settings":{"native-libraries":{"zstd":{
        "version":"1.5","license":"BSD-3-Clause","link":"magic"}}}
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m);
    auto libs = parseNativeLibraries(*m);
    ASSERT_FALSE((bool) libs);
    EXPECT_NE(errorText(libs.takeError()).find("unknown link mode"),
              std::string::npos);
}

// 1.1.3 — native-overrides: version form.
TEST(NativeDepsManifestTests, parsesOverrideVersion) {
    std::string src = R"({
      "details":{"name":"a.b","version":"0.1"},
      "settings":{"native-overrides":{"zstd":{"version":"1.5.6"}}}
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m);
    auto ov = parseNativeOverrides(*m);
    ASSERT_TRUE((bool) ov) << errorText(ov.takeError());
    ASSERT_EQ(ov->size(), 1u);
    EXPECT_EQ(ov->at("zstd").version.value_or(""), "1.5.6");
    EXPECT_FALSE(ov->at("zstd").path.has_value());
}

// 1.1.3 — native-overrides: path form.
TEST(NativeDepsManifestTests, parsesOverridePath) {
    std::string src = R"({
      "details":{"name":"a.b","version":"0.1"},
      "settings":{"native-overrides":{"zstd":{"path":"/opt/zstd/libzstd.a"}}}
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m);
    auto ov = parseNativeOverrides(*m);
    ASSERT_TRUE((bool) ov) << errorText(ov.takeError());
    EXPECT_EQ(ov->at("zstd").path.value_or(""), "/opt/zstd/libzstd.a");
}

// 1.1.3 — override with neither version nor path fails loud.
TEST(NativeDepsManifestTests, rejectsEmptyOverride) {
    std::string src = R"({
      "details":{"name":"a.b","version":"0.1"},
      "settings":{"native-overrides":{"zstd":{}}}
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m);
    auto ov = parseNativeOverrides(*m);
    ASSERT_FALSE((bool) ov);
    EXPECT_NE(errorText(ov.takeError()).find("must set 'version' or 'path'"),
              std::string::npos);
}

// Missing blocks → empty maps, no error.
TEST(NativeDepsManifestTests, missingBlocksYieldEmpty) {
    std::string src = R"({"details":{"name":"a.b","version":"0.1"}})";
    auto m = loadManifestString(src);
    ASSERT_TRUE((bool) m);
    auto libs = parseNativeLibraries(*m);
    ASSERT_TRUE((bool) libs);
    EXPECT_TRUE(libs->empty());
    auto ov = parseNativeOverrides(*m);
    ASSERT_TRUE((bool) ov);
    EXPECT_TRUE(ov->empty());
}
