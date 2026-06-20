// Native-deps unit 4 — requirement model + transitive collection.
// See native-deps-plan.md unit 4, spec §4.1.

#include "cajeta/buildtool/NativeResolver.h"
#include "cajeta/compile/CajetaArchive.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <string>
#include <vector>

using cajeta::CajetaArchive;
using cajeta::buildtool::collectNativeRequirements;

namespace {
std::vector<uint8_t> bytesOf(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}
// Build an in-memory archive carrying the given native-meta json.
CajetaArchive archiveWithMeta(const std::string& name, const std::string& meta) {
    CajetaArchive a(name, "1.0", CajetaArchive::Kind::Cja);
    if (!meta.empty()) a.setNativeLibrariesMeta(bytesOf(meta));
    return a;
}
std::string errText(llvm::Error&& e) {
    std::string s; llvm::raw_string_ostream os(s); os << e;
    consumeError(std::move(e)); return s;
}
} // namespace

// 4.1.1 — union across two deps each declaring a distinct native req.
TEST(NativeResolverCollectTests, unionsTwoDeps) {
    auto a = archiveWithMeta("dep.a", R"({
        "requires":["zstd"],
        "libraries":{"zstd":{"version":"1.5.*","license":"BSD-3-Clause",
                             "redistributable":true}}})");
    auto b = archiveWithMeta("dep.b", R"({
        "requires":["lz4"],
        "libraries":{"lz4":{"version":"1.9.*","license":"BSD-2-Clause",
                            "redistributable":true}}})");
    std::vector<const CajetaArchive*> arcs{&a, &b};
    auto set = collectNativeRequirements(arcs);
    ASSERT_TRUE((bool) set) << errText(set.takeError());
    EXPECT_EQ(set->required.size(), 2u);
    EXPECT_EQ(set->libraries.size(), 2u);
    EXPECT_TRUE(set->libraries.count("zstd"));
    EXPECT_TRUE(set->libraries.count("lz4"));
    EXPECT_TRUE(set->unsatisfied.empty());
}

// 4.1.2 — same lib from two deps: deduped, both version constraints preserved.
TEST(NativeResolverCollectTests, dedupsAndKeepsVersionConstraints) {
    auto a = archiveWithMeta("dep.a", R"({
        "requires":["zstd"],
        "libraries":{"zstd":{"version":"1.5.2","license":"BSD-3-Clause"}}})");
    auto b = archiveWithMeta("dep.b", R"({
        "requires":["zstd"],
        "libraries":{"zstd":{"version":"1.5.6","license":"BSD-3-Clause"}}})");
    std::vector<const CajetaArchive*> arcs{&a, &b};
    auto set = collectNativeRequirements(arcs);
    ASSERT_TRUE((bool) set) << errText(set.takeError());
    EXPECT_EQ(set->libraries.size(), 1u);          // deduped
    ASSERT_EQ(set->versionConstraints.at("zstd").size(), 2u);  // both kept
    EXPECT_TRUE(set->unsatisfied.empty());
}

// 4.1.3 — a required lib with no resolution metadata anywhere is unsatisfied.
TEST(NativeResolverCollectTests, requiredWithNoProviderIsUnsatisfied) {
    // dep.a requires zstd but provides no libraries metadata; nobody else does.
    auto a = archiveWithMeta("dep.a", R"({"requires":["zstd"],"libraries":{}})");
    std::vector<const CajetaArchive*> arcs{&a};
    auto set = collectNativeRequirements(arcs);
    ASSERT_TRUE((bool) set) << errText(set.takeError());
    ASSERT_EQ(set->unsatisfied.size(), 1u);
    EXPECT_TRUE(set->unsatisfied.count("zstd"));
}

// 4.3.1 — collection is order-independent (provider in a different dep).
TEST(NativeResolverCollectTests, orderIndependentCrossDepProvider) {
    // dep.user requires zstd; dep.prov provides the metadata.
    auto user = archiveWithMeta("dep.user", R"({"requires":["zstd"],"libraries":{}})");
    auto prov = archiveWithMeta("dep.prov", R"({
        "libraries":{"zstd":{"version":"1.5.*","license":"BSD-3-Clause"}}})");
    std::vector<const CajetaArchive*> fwd{&user, &prov};
    std::vector<const CajetaArchive*> rev{&prov, &user};
    auto s1 = collectNativeRequirements(fwd);
    auto s2 = collectNativeRequirements(rev);
    ASSERT_TRUE((bool) s1); ASSERT_TRUE((bool) s2);
    EXPECT_TRUE(s1->unsatisfied.empty());          // provider satisfies it
    EXPECT_TRUE(s2->unsatisfied.empty());
    EXPECT_EQ(s1->libraries.size(), s2->libraries.size());
    EXPECT_TRUE(s1->libraries.count("zstd"));
    EXPECT_TRUE(s2->libraries.count("zstd"));
}

// Slim archives (no native metadata) contribute nothing, no error.
TEST(NativeResolverCollectTests, slimArchivesContributeNothing) {
    auto slim = archiveWithMeta("dep.slim", "");
    std::vector<const CajetaArchive*> arcs{&slim};
    auto set = collectNativeRequirements(arcs);
    ASSERT_TRUE((bool) set);
    EXPECT_TRUE(set->required.empty());
    EXPECT_TRUE(set->libraries.empty());
    EXPECT_TRUE(set->unsatisfied.empty());
}

// Malformed embedded metadata fails loud.
TEST(NativeResolverCollectTests, malformedMetaFailsLoud) {
    auto bad = archiveWithMeta("dep.bad", R"({"libraries":{"zstd":{"license":"X"}}})");
    std::vector<const CajetaArchive*> arcs{&bad};
    auto set = collectNativeRequirements(arcs);
    ASSERT_FALSE((bool) set);
    EXPECT_NE(errText(set.takeError()).find("missing required 'version'"),
              std::string::npos);
}
