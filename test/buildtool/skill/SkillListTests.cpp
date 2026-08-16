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

// D.5b.1 / §3.2 — scope is an exact prefix-inclusive subtree, not fuzzy.

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
