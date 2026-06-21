// Tests for the CLI adapter helpers + context loader.
// See src/cajeta/buildtool/skill/SkillCli.h and
// docs/specs/skill-discovery-spec.md §1.5.1 (plan unit D.7).

#include "cajeta/buildtool/skill/SkillCli.h"

#include "cajeta/buildtool/skill/SkillPackager.h"
#include "cajeta/compile/CajetaArchive.h"

#include <gtest/gtest.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>

#include <fstream>
#include <map>
#include <optional>
#include <string>

using namespace cajeta::buildtool::skill;
using cajeta::CajetaArchive;
using cajeta::buildtool::ResolvedPackageEntry;

namespace {

    std::vector<std::string> A(std::vector<std::string> v) { return v; }

} // namespace

// D.7.1 — search-skill arg parsing (name + flags, both --flag value and =forms).
TEST(SkillCliTests, parseSearchArgs) {
    auto a = parseSearchSkillArgs(
        A({"search-skill", "cajeta/io/File", "--version", "1.2", "--from=app", "--exact"}));
    EXPECT_TRUE(a.valid);
    EXPECT_EQ(a.name, "cajeta/io/File");
    ASSERT_TRUE(a.version.has_value());
    EXPECT_EQ(*a.version, "1.2");
    ASSERT_TRUE(a.from.has_value());
    EXPECT_EQ(*a.from, "app");
    EXPECT_TRUE(a.exact);

    // Missing name → invalid (usage).
    EXPECT_FALSE(parseSearchSkillArgs(A({"search-skill", "--exact"})).valid);
}

// D.7.1 — list-skills arg parsing (scope optional).
TEST(SkillCliTests, parseListArgs) {
    auto none = parseListSkillsArgs(A({"list-skills"}));
    EXPECT_TRUE(none.valid);
    EXPECT_FALSE(none.scope.has_value());

    auto scoped = parseListSkillsArgs(A({"list-skills", "cajeta/io", "--version=2.0"}));
    EXPECT_TRUE(scoped.valid);
    ASSERT_TRUE(scoped.scope.has_value());
    EXPECT_EQ(*scoped.scope, "cajeta/io");
    EXPECT_EQ(*scoped.version, "2.0");
}

// D.7.1 — get-skills splits a comma-delimited URI list.
TEST(SkillCliTests, splitCommaUris) {
    auto v = splitCommaUris("a://x , b://y,, c://z ");
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0], "a://x");
    EXPECT_EQ(v[1], "b://y");
    EXPECT_EQ(v[2], "c://z");
}

// D.7.1 — output formatting.
TEST(SkillCliTests, formatting) {
    std::vector<SkillSearchResult> sr = {
        {"cja-skill://l@1/x", "cajeta/io/File", MatchSource::Name,
         MatchTier::Exact, 0}};
    EXPECT_EQ(formatSearchResults(sr), "cja-skill://l@1/x\tcajeta/io/File\n");

    std::vector<SkillListEntry> le = {{"cja-skill://l@1/x", {"cajeta/io/File"}, "Files"}};
    EXPECT_EQ(formatListEntries(le), "cja-skill://l@1/x\tFiles\n");
}

// D.7.1 — loadSkillSearchContext reads index.json from cached archives and wires
// moduleVersions from memberOwner.
TEST(SkillCliTests, loadsContextFromArchives) {
    // Build a real .cja with a skill index.
    llvm::SmallString<128> root;
    llvm::sys::fs::createUniqueDirectory("cajeta-cli", root);
    auto at = [&](llvm::StringRef rel) {
        llvm::SmallString<256> p(root);
        llvm::sys::path::append(p, rel);
        return p.str().str();
    };
    {
        std::string skill = at("pkg/skills/a.md");
        llvm::sys::fs::create_directories(llvm::sys::path::parent_path(skill));
        std::ofstream(skill) << "---\nid: alpha\napplies-to: [cajeta/io/File]\n---\nx\n";
    }
    CajetaArchive arc("cajeta.io", "1.0.0", CajetaArchive::Kind::Cja);
    ASSERT_FALSE((bool)addSkillMembersToArchive(arc, at("pkg")));
    std::string cja = at("cajeta.io.cja");
    arc.writeTo(cja);

    ResolvedPackageEntry e;
    e.name = "cajeta.io";
    e.version = "1.0.0";
    e.checksum = "sha256:x";
    e.memberOwner = "app";
    std::vector<ResolvedPackageEntry> pkgs = {e};

    auto lookup = [&](llvm::StringRef sum) -> std::optional<std::string> {
        return sum == "sha256:x" ? std::optional<std::string>(cja) : std::nullopt;
    };

    auto ctx = loadSkillSearchContext(pkgs, lookup);
    ASSERT_TRUE((bool)ctx);
    // The context is always seeded with the embedded stdlib archives (spec
    // §2.5), so the lockfile-resolved cajeta.io@1.0.0 is found among them
    // (distinct from the embedded stdlib cajeta.io@1.0).
    EXPECT_GT(ctx->archives.size(), 1u);
    bool foundResolved = false;
    for (const auto& a : ctx->archives) {
        if (a.library == "cajeta.io" && a.version == "1.0.0") {
            foundResolved = true;
            auto ids = a.index.query("cajeta/io/File", false);
            ASSERT_EQ(ids.size(), 1u);
            EXPECT_EQ(ids[0], "alpha");
        }
    }
    EXPECT_TRUE(foundResolved) << "lockfile-resolved cajeta.io@1.0.0 must be present";
    // memberOwner → moduleVersions.
    EXPECT_EQ(ctx->moduleVersions["app"]["cajeta.io"], "1.0.0");

    llvm::sys::fs::remove_directories(root);
}
