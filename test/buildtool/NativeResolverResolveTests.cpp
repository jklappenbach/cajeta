// Native-deps unit 5 — resolution: probe order, version selection, override.
// See native-deps-plan.md unit 5, spec §4.1/§4.3.

#include "cajeta/buildtool/NativeResolver.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

using namespace cajeta::buildtool;

namespace {
std::string errText(llvm::Error&& e) {
    std::string s; llvm::raw_string_ostream os(s); os << e;
    consumeError(std::move(e)); return s;
}
NativeRequirementSet reqZstd(std::vector<std::string> constraints) {
    NativeRequirementSet r;
    r.required.insert("zstd");
    if (!constraints.empty()) r.versionConstraints["zstd"] = std::move(constraints);
    NativeLibrary lib; lib.id = "zstd"; lib.link = "static";
    r.libraries["zstd"] = lib;
    return r;
}
NativeProvider always(const std::string& name, const std::string& path) {
    return NativeProvider{name, [path](const std::string&, const std::string&,
        const std::string&) -> std::optional<std::string> { return path; }};
}
NativeProvider never(const std::string& name) {
    return NativeProvider{name, [](const std::string&, const std::string&,
        const std::string&) -> std::optional<std::string> { return std::nullopt; }};
}
} // namespace

// 5.1.2 — highest-compatible version selection.
TEST(NativeResolverResolveTests, selectsHighestCompatibleVersion) {
    auto v = selectNativeVersion({"1.5.2", "1.5.6", "1.5.1"});
    ASSERT_TRUE((bool) v) << errText(v.takeError());
    EXPECT_EQ(*v, "1.5.6");
}

// 5.1.2 — incompatible majors fail loud.
TEST(NativeResolverResolveTests, incompatibleMajorsFail) {
    auto v = selectNativeVersion({"1.5.2", "2.0.0"});
    ASSERT_FALSE((bool) v);
    EXPECT_NE(errText(v.takeError()).find("incompatible"), std::string::npos);
}

// 5.1.1 — probe order: first provider that supplies wins.
TEST(NativeResolverResolveTests, probeOrderFirstHitWins) {
    auto reqs = reqZstd({"1.5.*"});
    auto res = resolveNatives(reqs, {}, "linux-x64",
                              {always("cja", "cja/zstd.a"),
                               always("project", "proj/zstd.a")});
    ASSERT_TRUE(res.resolved.count("zstd"));
    EXPECT_EQ(res.resolved.at("zstd").via, "cja");
    EXPECT_EQ(res.resolved.at("zstd").artifactPath, "cja/zstd.a");
    EXPECT_TRUE(res.unresolved.empty());
}

// 5.1.1 — falls through to a later provider when earlier ones decline.
TEST(NativeResolverResolveTests, fallsThroughToLaterProvider) {
    auto reqs = reqZstd({"1.5.*"});
    auto res = resolveNatives(reqs, {}, "linux-x64",
                              {never("cja"), never("cache"),
                               always("project", "proj/zstd.a")});
    ASSERT_TRUE(res.resolved.count("zstd"));
    EXPECT_EQ(res.resolved.at("zstd").via, "project");
}

// 5.1.1 — no provider supplies → unresolved with a reason.
TEST(NativeResolverResolveTests, noProviderUnresolved) {
    auto reqs = reqZstd({"1.5.*"});
    auto res = resolveNatives(reqs, {}, "linux-x64", {never("cja")});
    EXPECT_TRUE(res.resolved.empty());
    ASSERT_TRUE(res.unresolved.count("zstd"));
    EXPECT_NE(res.unresolved.at("zstd").find("no provider"), std::string::npos);
}

// 5.1.3 — override path short-circuits the provider chain.
TEST(NativeResolverResolveTests, overridePathShortCircuits) {
    auto reqs = reqZstd({"1.5.*"});
    std::map<std::string, NativeOverride> ov;
    NativeOverride o; o.path = "/opt/zstd/libzstd.a"; ov["zstd"] = o;
    auto guard = NativeProvider{"cja", [](const std::string&, const std::string&,
        const std::string&) -> std::optional<std::string> {
        ADD_FAILURE() << "provider must not be consulted with an override path";
        return std::nullopt; }};
    auto res = resolveNatives(reqs, ov, "linux-x64", {guard});
    ASSERT_TRUE(res.resolved.count("zstd"));
    EXPECT_EQ(res.resolved.at("zstd").artifactPath, "/opt/zstd/libzstd.a");
    EXPECT_EQ(res.resolved.at("zstd").via, "override-path");
}

// 5.1.3 — override version replaces the constraint passed to providers.
TEST(NativeResolverResolveTests, overrideVersionWins) {
    auto reqs = reqZstd({"1.5.2"});
    std::map<std::string, NativeOverride> ov;
    NativeOverride o; o.version = "9.9.9"; ov["zstd"] = o;
    std::string seen;
    NativeProvider cap{"cja", [&seen](const std::string&, const std::string& v,
        const std::string&) -> std::optional<std::string> {
        seen = v; return std::optional<std::string>("x"); }};
    auto res = resolveNatives(reqs, ov, "linux-x64", {cap});
    EXPECT_EQ(seen, "9.9.9");
    ASSERT_TRUE(res.resolved.count("zstd"));
    EXPECT_EQ(res.resolved.at("zstd").version, "9.9.9");
}

// 5.3.1 — a required lib with no metadata and no override is unresolved.
TEST(NativeResolverResolveTests, unsatisfiedNoOverrideUnresolved) {
    NativeRequirementSet reqs;
    reqs.required.insert("zstd");   // no constraints, no libraries, no override
    auto res = resolveNatives(reqs, {}, "linux-x64", {always("cja", "x")});
    EXPECT_TRUE(res.resolved.empty());
    ASSERT_TRUE(res.unresolved.count("zstd"));
    EXPECT_NE(res.unresolved.at("zstd").find("unsatisfied"), std::string::npos);
}
