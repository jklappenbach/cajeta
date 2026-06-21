// Tests for the Get core.
// See src/cajeta/buildtool/skill/SkillGet.h and
// docs/specs/skill-discovery-spec.md §2.1 (plan unit D.6).

#include "cajeta/buildtool/skill/SkillGet.h"

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

using cajeta::CajetaArchive;
using cajeta::buildtool::ResolvedPackageEntry;
using cajeta::buildtool::skill::addSkillMembersToArchive;
using cajeta::buildtool::skill::getSkills;
using cajeta::buildtool::skill::SkillGetResult;

namespace {

    struct TempDir {
        llvm::SmallString<128> root;
        TempDir() { llvm::sys::fs::createUniqueDirectory("cajeta-get", root); }
        ~TempDir() { llvm::sys::fs::remove_directories(root); }
        std::string at(llvm::StringRef rel) const {
            llvm::SmallString<256> p(root);
            llvm::sys::path::append(p, rel);
            return p.str().str();
        }
    };

    void writeFile(llvm::StringRef path, llvm::StringRef content) {
        llvm::sys::fs::create_directories(llvm::sys::path::parent_path(path));
        std::ofstream os(path.str(), std::ios::binary);
        os << content.str();
    }

    // Package a single skill into a real .cja at `cjaPath`.
    void packageOne(const std::string& pkgRoot, llvm::StringRef skillRel,
                    llvm::StringRef skillBytes, const std::string& cjaPath,
                    llvm::StringRef lib, llvm::StringRef ver) {
        writeFile(pkgRoot + "/" + skillRel.str(), skillBytes);
        CajetaArchive arc(lib.str(), ver.str(), CajetaArchive::Kind::Cja);
        EXPECT_FALSE((bool)addSkillMembersToArchive(arc, pkgRoot));
        arc.writeTo(cjaPath);
    }

    ResolvedPackageEntry pkg(std::string name, std::string version,
                             std::string checksum) {
        ResolvedPackageEntry e;
        e.name = std::move(name);
        e.version = std::move(version);
        e.checksum = std::move(checksum);
        return e;
    }

} // namespace

// D.6.1 / D.6.3 — batch across different libraries/versions; payload byte-identical
// to what was packaged; keyed back to each input URI.
TEST(SkillGetTests, batchAcrossArchivesByteIdentical) {
    TempDir tmp;
    const std::string aBytes =
        "---\nid: alpha\napplies-to: [cajeta/io/A]\n---\nAlpha payload.\n";
    const std::string bBytes =
        "---\nid: beta\napplies-to: [cajeta/net/B]\n---\nBeta payload.\n";
    std::string cjaA = tmp.at("a.cja");
    std::string cjaB = tmp.at("b.cja");
    packageOne(tmp.at("pkgA"), "skills/a.md", aBytes, cjaA, "cajeta.io", "1.0.0");
    packageOne(tmp.at("pkgB"), "skills/b.md", bBytes, cjaB, "cajeta.net", "2.0.0");

    std::vector<ResolvedPackageEntry> pkgs = {
        pkg("cajeta.io", "1.0.0", "sha256:a"),
        pkg("cajeta.net", "2.0.0", "sha256:b"),
    };
    std::map<std::string, std::string> cache = {
        {"sha256:a", cjaA}, {"sha256:b", cjaB}};
    auto lookup = [&](llvm::StringRef sum) -> std::optional<std::string> {
        auto it = cache.find(sum.str());
        return it == cache.end() ? std::nullopt
                                 : std::optional<std::string>(it->second);
    };

    std::vector<std::string> uris = {
        "cja-skill://cajeta.io@1.0.0/alpha",
        "cja-skill://cajeta.net@2.0.0/beta",
    };
    auto r = getSkills(uris, pkgs, lookup);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].uri, uris[0]);
    EXPECT_TRUE(r[0].ok());
    EXPECT_EQ(r[0].payload, aBytes);
    EXPECT_EQ(r[1].uri, uris[1]);
    EXPECT_TRUE(r[1].ok());
    EXPECT_EQ(r[1].payload, bBytes);
}

// D.6.1 — a bad/missing URI in the batch is a per-URI error; others still resolve.
TEST(SkillGetTests, perUriErrorsDoNotFailBatch) {
    TempDir tmp;
    const std::string aBytes =
        "---\nid: alpha\napplies-to: [cajeta/io/A]\n---\nAlpha.\n";
    std::string cjaA = tmp.at("a.cja");
    packageOne(tmp.at("pkgA"), "skills/a.md", aBytes, cjaA, "cajeta.io", "1.0.0");

    std::vector<ResolvedPackageEntry> pkgs = {
        pkg("cajeta.io", "1.0.0", "sha256:a")};
    auto lookup = [&](llvm::StringRef sum) -> std::optional<std::string> {
        if (sum == "sha256:a") return cjaA;
        return std::nullopt;
    };

    std::vector<std::string> uris = {
        "cja-skill://cajeta.io@1.0.0/alpha",   // ok
        "not-a-uri",                            // malformed
        "cja-skill://cajeta.io@9.9.9/alpha",    // unresolved version
        "cja-skill://cajeta.io@1.0.0/missing",  // resolves, no such member
    };
    auto r = getSkills(uris, pkgs, lookup);
    ASSERT_EQ(r.size(), 4u);
    EXPECT_TRUE(r[0].ok());
    EXPECT_EQ(r[0].payload, aBytes);
    EXPECT_FALSE(r[1].ok());
    EXPECT_FALSE(r[2].ok());
    EXPECT_NE(r[2].error.find("9.9.9"), std::string::npos);
    EXPECT_FALSE(r[3].ok());
    EXPECT_NE(r[3].error.find("missing"), std::string::npos);
}
