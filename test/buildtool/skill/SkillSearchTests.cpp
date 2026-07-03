// Tests for the Search core.
// See src/cajeta/buildtool/skill/SkillSearch.h and
// specs/archive/skill-discovery-spec.md §3.2–§3.5 (plan unit D.5).

#include "cajeta/buildtool/skill/SkillSearch.h"

#include "cajeta/buildtool/skill/SkillDocument.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using cajeta::buildtool::skill::MatchOptions;
using cajeta::buildtool::skill::MatchSource;
using cajeta::buildtool::skill::MatchTier;
using cajeta::buildtool::skill::ResolvedSkillArchive;
using cajeta::buildtool::skill::searchSkills;
using cajeta::buildtool::skill::SkillDocument;
using cajeta::buildtool::skill::SkillIndex;
using cajeta::buildtool::skill::SkillSearchContext;
using cajeta::buildtool::skill::SkillSearchResult;

namespace {

    SkillDocument doc(std::string id, std::vector<std::string> appliesTo,
                      std::string title = "") {
        SkillDocument d;
        d.id = std::move(id);
        d.appliesTo = std::move(appliesTo);
        d.title = std::move(title);
        d.body = "b";
        return d;
    }

    ResolvedSkillArchive archive(std::string lib, std::string ver,
                                 std::vector<SkillDocument> docs) {
        auto idx = SkillIndex::build(docs);
        EXPECT_TRUE((bool)idx);
        ResolvedSkillArchive a;
        a.library = std::move(lib);
        a.version = std::move(ver);
        if (idx) a.index = std::move(*idx);
        return a;
    }

    std::set<std::string> uris(const std::vector<SkillSearchResult>& r) {
        std::set<std::string> s;
        for (const auto& x : r) s.insert(x.uri);
        return s;
    }

    const SkillSearchResult* byUri(const std::vector<SkillSearchResult>& r,
                                   llvm::StringRef u) {
        for (const auto& x : r)
            if (x.uri == u) return &x;
        return nullptr;
    }

} // namespace

// D.5.1 / §3.2 — hierarchical, prefix-inclusive results.
TEST(SkillSearchTests, hierarchicalPrefixInclusive) {
    SkillSearchContext ctx;
    ctx.archives.push_back(archive("cajeta.torch", "1.0.0", {
        doc("nn-overview", {"cajeta/torch/nn"}),
        doc("linear", {"cajeta/torch/nn/Linear"}),
        doc("forward", {"cajeta/torch/nn/Linear.forward"}),
    }));

    auto r = searchSkills("cajeta/torch/nn", std::nullopt, std::nullopt, ctx);
    EXPECT_EQ(uris(r), (std::set<std::string>{
        "cja-skill://cajeta.torch@1.0.0/nn-overview",
        "cja-skill://cajeta.torch@1.0.0/linear",
        "cja-skill://cajeta.torch@1.0.0/forward"}));
    // The exact (overview) result ranks first.
    EXPECT_EQ(r.front().uri, "cja-skill://cajeta.torch@1.0.0/nn-overview");
    EXPECT_EQ(r.front().tier, MatchTier::Exact);

    // A deep query surfaces the nearest-ancestor overview.
    auto deep = searchSkills("cajeta/torch/nn/Linear.forward", std::nullopt,
                             std::nullopt, ctx);
    EXPECT_NE(byUri(deep, "cja-skill://cajeta.torch@1.0.0/forward"), nullptr);
    const auto* anc = byUri(deep, "cja-skill://cajeta.torch@1.0.0/linear");
    ASSERT_NE(anc, nullptr);
    EXPECT_EQ(anc->tier, MatchTier::AncestorOverview);
}

// D.5.1 / §3.3 — assembled across archives; each contributes only its symbols.
TEST(SkillSearchTests, firstPartyAcrossArchives) {
    SkillSearchContext ctx;
    ctx.archives.push_back(
        archive("cajeta.io", "1.0.0", {doc("f", {"cajeta/io/File"})}));
    ctx.archives.push_back(
        archive("other.net", "3.0.0", {doc("n", {"other/net/Conn"})}));

    auto r = searchSkills("cajeta/io/File", std::nullopt, std::nullopt, ctx);
    EXPECT_EQ(uris(r),
              (std::set<std::string>{"cja-skill://cajeta.io@1.0.0/f"}));
}

// D.5.1 — empty result when nothing matches within threshold.
TEST(SkillSearchTests, emptyOnNoMatch) {
    SkillSearchContext ctx;
    ctx.archives.push_back(
        archive("cajeta.io", "1.0.0", {doc("f", {"cajeta/io/File"})}));
    EXPECT_TRUE(searchSkills("zzz/qqq/Wwww", std::nullopt, std::nullopt, ctx).empty());
}

// D.5.1 / §3.4 — version key restricts to that resolved version.
TEST(SkillSearchTests, versionKeyRestricts) {
    SkillSearchContext ctx;
    ctx.archives.push_back(
        archive("foo", "1.0.0", {doc("fo", {"foo/io/File"})}));
    ctx.archives.push_back(
        archive("foo", "2.0.0", {doc("fo", {"foo/io/File"})}));

    auto r = searchSkills("foo/io/File", std::string("2.0.0"), std::nullopt, ctx);
    EXPECT_EQ(uris(r), (std::set<std::string>{"cja-skill://foo@2.0.0/fo"}));
}

// D.5.1 / §3.4 — diamond: version omitted → both versions, tagged. No silent pick.
TEST(SkillSearchTests, diamondNoVersionReturnsAllTagged) {
    SkillSearchContext ctx;
    ctx.archives.push_back(
        archive("foo", "1.0.0", {doc("fo", {"foo/io/File"})}));
    ctx.archives.push_back(
        archive("foo", "2.0.0", {doc("fo", {"foo/io/File"})}));

    auto r = searchSkills("foo/io/File", std::nullopt, std::nullopt, ctx);
    EXPECT_EQ(uris(r), (std::set<std::string>{
        "cja-skill://foo@1.0.0/fo", "cja-skill://foo@2.0.0/fo"}));
}

// D.5.1 / §3.5 — fuzzy resolves and reports the matched canonical name; exact off.
TEST(SkillSearchTests, fuzzyReportsMatchedName) {
    SkillSearchContext ctx;
    ctx.archives.push_back(
        archive("cajeta.io", "1.0.0", {doc("f", {"cajeta/io/File"}, "Opening files")}));

    auto r = searchSkills("cajeta/io/Fiel", std::nullopt, std::nullopt, ctx);
    const auto* m = byUri(r, "cja-skill://cajeta.io@1.0.0/f");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->matchedName, "cajeta/io/File");
    EXPECT_EQ(m->distance, 1);

    MatchOptions exact;
    exact.exact = true;
    EXPECT_TRUE(
        searchSkills("cajeta/io/Fiel", std::nullopt, std::nullopt, ctx, exact).empty());
}

// D.5.1 / §3.4 — consumer-scoped: `from` infers the version; version overrides from.
TEST(SkillSearchTests, consumerScopedFrom) {
    SkillSearchContext ctx;
    ctx.archives.push_back(
        archive("foo", "1.0.0", {doc("fo", {"foo/io/File"})}));
    ctx.archives.push_back(
        archive("foo", "2.0.0", {doc("fo", {"foo/io/File"})}));
    ctx.moduleVersions["app"] = {{"foo", "1.0.0"}};
    ctx.moduleVersions["lib"] = {{"foo", "2.0.0"}};

    auto fromApp = searchSkills("foo/io/File", std::nullopt, std::string("app"), ctx);
    EXPECT_EQ(uris(fromApp), (std::set<std::string>{"cja-skill://foo@1.0.0/fo"}));

    auto fromLib = searchSkills("foo/io/File", std::nullopt, std::string("lib"), ctx);
    EXPECT_EQ(uris(fromLib), (std::set<std::string>{"cja-skill://foo@2.0.0/fo"}));

    // Explicit version overrides from.
    auto override_ =
        searchSkills("foo/io/File", std::string("2.0.0"), std::string("app"), ctx);
    EXPECT_EQ(uris(override_), (std::set<std::string>{"cja-skill://foo@2.0.0/fo"}));
}

// D.5.3 — every returned URI carries its resolved version.
TEST(SkillSearchTests, everyUriCarriesResolvedVersion) {
    SkillSearchContext ctx;
    ctx.archives.push_back(
        archive("foo", "1.2.3", {doc("fo", {"foo/io/File"})}));
    auto r = searchSkills("foo/io/File", std::nullopt, std::nullopt, ctx);
    ASSERT_FALSE(r.empty());
    for (const auto& x : r)
        EXPECT_NE(x.uri.find("@1.2.3/"), std::string::npos);
}
