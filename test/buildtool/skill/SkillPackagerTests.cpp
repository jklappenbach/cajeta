// Tests for build-time skill packaging.
// See src/cajeta/buildtool/skill/SkillPackager.h and
// docs/specs/skill-discovery-spec.md §4.2 (plan unit D.3).

#include "cajeta/buildtool/skill/SkillPackager.h"

#include "cajeta/buildtool/skill/SkillIndex.h"
#include "cajeta/compile/CajetaArchive.h"

#include <gtest/gtest.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include <fstream>
#include <string>

using cajeta::CajetaArchive;
using cajeta::buildtool::skill::addSkillMembersToArchive;
using cajeta::buildtool::skill::buildSkillMembers;
using cajeta::buildtool::skill::SkillIndex;
using cajeta::buildtool::skill::SkillMember;

namespace {

    // A scratch package directory removed on destruction.
    struct TempPackage {
        llvm::SmallString<128> root;
        TempPackage() {
            llvm::sys::fs::createUniqueDirectory("cajeta-skillpkg", root);
        }
        ~TempPackage() { llvm::sys::fs::remove_directories(root); }

        void write(llvm::StringRef rel, llvm::StringRef content) {
            llvm::SmallString<256> p(root);
            llvm::sys::path::append(p, rel);
            llvm::sys::fs::create_directories(llvm::sys::path::parent_path(p));
            std::ofstream os(p.str().str(), std::ios::binary);
            os << content.str();
        }
        std::string path() const { return root.str().str(); }
    };

    std::vector<SkillMember> unwrap(llvm::Expected<std::vector<SkillMember>> e) {
        EXPECT_TRUE((bool)e);
        if (!e) {
            consumeError(e.takeError());
            return {};
        }
        return std::move(*e);
    }

    const SkillMember* find(const std::vector<SkillMember>& v, llvm::StringRef p) {
        for (const auto& m : v)
            if (m.path == p) return &m;
        return nullptr;
    }

} // namespace

// D.3.1 — a package with skills/ yields members incl. skills/index.json, and they
// round-trip through a real .cja.
TEST(SkillPackagerTests, packagesSkillsIntoArchive) {
    TempPackage pkg;
    pkg.write("skills/a.md",
              "---\nid: alpha\napplies-to: [cajeta/io/A]\ntitle: Aye\n---\nAlpha body.\n");
    pkg.write("skills/b.md",
              "---\nid: beta\napplies-to: [cajeta/io/B]\n---\nBeta body.\n");

    auto members = unwrap(buildSkillMembers(pkg.path()));
    ASSERT_NE(find(members, "skills/alpha.md"), nullptr);
    ASSERT_NE(find(members, "skills/beta.md"), nullptr);
    ASSERT_NE(find(members, "skills/index.json"), nullptr);
    EXPECT_EQ(find(members, "skills/alpha.md")->bytes,
              "---\nid: alpha\napplies-to: [cajeta/io/A]\ntitle: Aye\n---\nAlpha body.\n");

    // Write into a real archive, read it back, verify members + index.
    CajetaArchive arc("pkg", "1.0.0", CajetaArchive::Kind::Cja);
    ASSERT_FALSE((bool)addSkillMembersToArchive(arc, pkg.path()));

    llvm::SmallString<128> out;
    llvm::sys::fs::createUniqueDirectory("cajeta-cja", out);
    std::string cja = (out + "/pkg.cja").str();
    arc.writeTo(cja);

    CajetaArchive read = CajetaArchive::readFrom(cja);
    const auto* alpha = read.findEntry("skills/alpha.md");
    ASSERT_NE(alpha, nullptr);
    EXPECT_EQ(std::string(alpha->data.begin(), alpha->data.end()),
              "---\nid: alpha\napplies-to: [cajeta/io/A]\ntitle: Aye\n---\nAlpha body.\n");
    const auto* idxEntry = read.findEntry("skills/index.json");
    ASSERT_NE(idxEntry, nullptr);
    std::string idxJson(idxEntry->data.begin(), idxEntry->data.end());
    auto idx = SkillIndex::deserialize(idxJson);
    ASSERT_TRUE((bool)idx);
    auto ids = idx->query("cajeta/io/A", false);
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], "alpha");

    llvm::sys::fs::remove_directories(out);
}

// D.3.1 — an invalid skill fails the build with a diagnostic naming the file.
TEST(SkillPackagerTests, invalidSkillFailsBuild) {
    TempPackage pkg;
    pkg.write("skills/bad.md", "---\napplies-to: [cajeta/io/A]\n---\nNo id.\n");
    auto e = buildSkillMembers(pkg.path());
    ASSERT_FALSE((bool)e);
    std::string msg;
    llvm::raw_string_ostream os(msg);
    os << e.takeError();
    EXPECT_NE(msg.find("bad.md"), std::string::npos);
    EXPECT_NE(msg.find("id"), std::string::npos);
}

// D.3.1 — a package with no skills builds no members (no regression).
TEST(SkillPackagerTests, noSkillsIsEmpty) {
    TempPackage pkg;
    pkg.write("src/Main.cja", "// code, no skills\n");
    EXPECT_TRUE(unwrap(buildSkillMembers(pkg.path())).empty());

    // An empty skills/ dir is also empty (no index emitted).
    TempPackage pkg2;
    pkg2.write("skills/README.txt", "not a skill\n");
    EXPECT_TRUE(unwrap(buildSkillMembers(pkg2.path())).empty());
}

// D.3.3 — packaging is reproducible: identical member order + bytes across runs.
TEST(SkillPackagerTests, reproducibleOrdering) {
    TempPackage pkg;
    pkg.write("skills/z.md", "---\nid: zeta\napplies-to: [cajeta/io/Z]\n---\nZ.\n");
    pkg.write("skills/a.md", "---\nid: alpha\napplies-to: [cajeta/io/A]\n---\nA.\n");
    auto m1 = unwrap(buildSkillMembers(pkg.path()));
    auto m2 = unwrap(buildSkillMembers(pkg.path()));
    ASSERT_EQ(m1.size(), m2.size());
    for (size_t i = 0; i < m1.size(); ++i) {
        EXPECT_EQ(m1[i].path, m2[i].path);
        EXPECT_EQ(m1[i].bytes, m2[i].bytes);
    }
    // Sorted by path lexicographically (all share "skills/"): alpha, index, zeta.
    EXPECT_EQ(m1.front().path, "skills/alpha.md");
    EXPECT_EQ(m1.back().path, "skills/zeta.md");
}
