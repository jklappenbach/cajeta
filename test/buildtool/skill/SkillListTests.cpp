// Tests for the List core.
// See src/cajeta/buildtool/skill/SkillSearch.h and
// specs/archive/skill-discovery-spec.md §3.6 (plan unit D.5b).

#include "cajeta/buildtool/skill/SkillSearch.h"

#include "cajeta/buildtool/skill/SkillDocument.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using cajeta::buildtool::skill::listSkills;
using cajeta::buildtool::skill::ResolvedSkillArchive;
using cajeta::buildtool::skill::SkillDocument;
using cajeta::buildtool::skill::SkillIndex;
using cajeta::buildtool::skill::SkillListEntry;
using cajeta::buildtool::skill::SkillSearchContext;

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

    std::set<std::string> uris(const std::vector<SkillListEntry>& r) {
        std::set<std::string> s;
        for (const auto& x : r) s.insert(x.uri);
        return s;
    }

} // namespace

// D.5b.1 — no scope enumerates every skill (URI + bound names + title).
TEST(SkillListTests, listsAllNoScope) {
    SkillSearchContext ctx;
    ctx.archives.push_back(archive("cajeta.io", "1.0.0",
        {doc("f", {"cajeta/io/File"}, "Files")}));
    ctx.archives.push_back(archive("other.net", "2.0.0",
        {doc("n", {"other/net/Conn"}, "Conns")}));

    auto r = listSkills(std::nullopt, std::nullopt, std::nullopt, ctx);
    EXPECT_EQ(uris(r), (std::set<std::string>{
        "cja-skill://cajeta.io@1.0.0/f", "cja-skill://other.net@2.0.0/n"}));
    // Entry carries names + title.
    ASSERT_FALSE(r.empty());
    EXPECT_EQ(r.front().uri, "cja-skill://cajeta.io@1.0.0/f");
    EXPECT_EQ(r.front().names, (std::vector<std::string>{"cajeta/io/File"}));
    EXPECT_EQ(r.front().title, "Files");
}

// D.5b.1 / §3.2 — scope is an exact prefix-inclusive subtree, not fuzzy.
TEST(SkillListTests, scopedSubtreeExactPrefix) {
    SkillSearchContext ctx;
    ctx.archives.push_back(archive("cajeta.torch", "1.0.0", {
        doc("nn", {"cajeta/torch/nn"}),
        doc("lin", {"cajeta/torch/nn/Linear"}),
        doc("opt", {"cajeta/torch/optim/SGD"}),
    }));

    auto scoped = listSkills(std::string("cajeta/torch/nn"), std::nullopt,
                             std::nullopt, ctx);
    EXPECT_EQ(uris(scoped), (std::set<std::string>{
        "cja-skill://cajeta.torch@1.0.0/nn",
        "cja-skill://cajeta.torch@1.0.0/lin"}));

    // Not fuzzy: a misspelled scope matches nothing.
    EXPECT_TRUE(listSkills(std::string("cajeta/torch/Xn"), std::nullopt,
                           std::nullopt, ctx).empty());
}

// D.5b.1 / §3.4 — multi-version → version-tagged entries; from scopes the version.
TEST(SkillListTests, versionAndFrom) {
    SkillSearchContext ctx;
    ctx.archives.push_back(archive("foo", "1.0.0", {doc("fo", {"foo/io/File"})}));
    ctx.archives.push_back(archive("foo", "2.0.0", {doc("fo", {"foo/io/File"})}));
    ctx.moduleVersions["app"] = {{"foo", "1.0.0"}};

    auto all = listSkills(std::nullopt, std::nullopt, std::nullopt, ctx);
    EXPECT_EQ(uris(all), (std::set<std::string>{
        "cja-skill://foo@1.0.0/fo", "cja-skill://foo@2.0.0/fo"}));

    auto fromApp = listSkills(std::nullopt, std::nullopt, std::string("app"), ctx);
    EXPECT_EQ(uris(fromApp), (std::set<std::string>{"cja-skill://foo@1.0.0/fo"}));
}

// D.5b.1 — deterministic ordering.
TEST(SkillListTests, deterministicOrdering) {
    SkillSearchContext ctx;
    ctx.archives.push_back(archive("z.lib", "1.0.0", {doc("z", {"z/Z"})}));
    ctx.archives.push_back(archive("a.lib", "1.0.0", {doc("a", {"a/A"})}));
    auto r1 = listSkills(std::nullopt, std::nullopt, std::nullopt, ctx);
    auto r2 = listSkills(std::nullopt, std::nullopt, std::nullopt, ctx);
    ASSERT_EQ(r1.size(), r2.size());
    for (size_t i = 0; i < r1.size(); ++i) EXPECT_EQ(r1[i].uri, r2[i].uri);
    // Sorted by URI: a.lib before z.lib.
    EXPECT_EQ(r1.front().uri, "cja-skill://a.lib@1.0.0/a");
}
