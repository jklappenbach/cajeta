// Tests for build-time skill packaging.
// See src/cajeta/buildtool/skill/SkillPackager.h and
// specs/archive/skill-discovery-spec.md §4.2 (plan unit D.3).

#include "cajeta/buildtool/skill/SkillPackager.h"

#include "cajeta/buildtool/skill/SkillIndex.h"
#include "cajeta/compile/CajetaArchive.h"

#include <gtest/gtest.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>

using cajeta::CajetaArchive;
using cajeta::buildtool::skill::addSkillMembersToArchive;
using cajeta::buildtool::skill::buildSkillMembers;
using cajeta::buildtool::skill::misplacedSkillWarnings;
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

// F.1.1 — compression protocol (spec §2.4): skill members are stored
// zstd-compressed in the .cja and returned decompressed by the reader. Guards
// that skill members ride the archive's entry-compression path unchanged.
TEST(SkillPackagerTests, skillMembersStoredCompressedDecompressedOnRead) {
    TempPackage pkg;
    // Large, highly compressible body so zstd definitely shrinks it and the
    // plaintext cannot appear verbatim on disk.
    const std::string sentinel = "CAJETA_SKILL_COMPRESSION_SENTINEL repeated line\n";
    std::string body;
    for (int i = 0; i < 4000; ++i) body += sentinel;
    const std::string full =
        "---\nid: big\napplies-to: [cajeta/io/Big]\ntitle: Big\n---\n" + body;
    pkg.write("skills/big.md", full);

    CajetaArchive arc("pkg", "1.0.0", CajetaArchive::Kind::Cja);
    ASSERT_FALSE((bool)addSkillMembersToArchive(arc, pkg.path()));

    llvm::SmallString<128> out;
    llvm::sys::fs::createUniqueDirectory("cajeta-cja", out);
    std::string cja = (out + "/pkg.cja").str();
    arc.writeTo(cja);

    // Raw on-disk bytes.
    std::ifstream in(cja, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    in.close();
    ASSERT_GE(raw.size(), (size_t) 16);

    // (a) the entries-compressed flag is set (flags u32 at header offset 12, LE).
    uint32_t flags = (uint8_t) raw[12] | ((uint8_t) raw[13] << 8)
                   | ((uint8_t) raw[14] << 16) | ((uint8_t) raw[15] << 24);
    EXPECT_NE(flags & 0x2u, 0u) << "FLAG_ENTRIES_COMPRESSED must be set";

    // (b) genuinely compressed on disk: plaintext sentinel absent, file tiny.
    EXPECT_EQ(raw.find(sentinel), std::string::npos)
        << "skill body must be compressed, not stored verbatim";
    EXPECT_LT(raw.size(), body.size() / 4);

    // (c) transparent decompression on read.
    CajetaArchive read = CajetaArchive::readFrom(cja);
    const auto* e = read.findEntry("skills/big.md");
    ASSERT_NE(e, nullptr);
    std::string got(e->data.begin(), e->data.end());
    EXPECT_EQ(got, full) << "reader must return the decompressed payload verbatim";

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

// Skills the packager will never see are reported (2026-10-21: codec 0.9.0,
// crypto 0.1.0 and cloud's identity skill all shipped without them).
namespace {
    const char* kSkill =
        "---\nid: s\napplies-to: [a/B]\n---\n# S\n";

    bool anyMentions(const std::vector<std::string>& ws, const std::string& needle) {
        for (const auto& w : ws)
            if (w.find(needle) != std::string::npos) return true;
        return false;
    }
}

TEST(SkillPackagerTests, skillsUnderAPackageDirectoryAreReported) {
    TempPackage pkg;
    pkg.write("cajeta.json", "{}\n");
    pkg.write("src/main/cajeta/a/skills/s.md", kSkill);
    auto ws = misplacedSkillWarnings(pkg.path(), pkg.path() + "/src/main/cajeta");
    ASSERT_EQ(ws.size(), 1u);
    EXPECT_TRUE(anyMentions(ws, "src/main/cajeta/a/skills")) << ws[0];
    EXPECT_TRUE(anyMentions(ws, "not packed")) << ws[0];
}

TEST(SkillPackagerTests, projectSkillsMissedByASourceRootSkillRootAreReported) {
    // The bare `cajeta --emit=cja ... src/main/cajeta` form: the skill root is
    // the source root, and the project's skills/ sits two levels up.
    TempPackage pkg;
    pkg.write("cajeta.json", "{}\n");
    pkg.write("skills/s.md", kSkill);
    pkg.write("src/main/cajeta/a/B.cajeta", "package a;\n");
    std::string src = pkg.path() + "/src/main/cajeta";
    auto ws = misplacedSkillWarnings(src, src);
    ASSERT_EQ(ws.size(), 1u);
    EXPECT_TRUE(anyMentions(ws, "--skill-root=")) << ws[0];
}

TEST(SkillPackagerTests, correctlyPlacedSkillsReportNothing) {
    TempPackage pkg;
    pkg.write("cajeta.json", "{}\n");
    pkg.write("skills/s.md", kSkill);
    pkg.write("src/main/cajeta/a/B.cajeta", "package a;\n");
    EXPECT_TRUE(misplacedSkillWarnings(pkg.path(), pkg.path() + "/src/main/cajeta").empty());

    // A project with no skills anywhere reports nothing from either form.
    TempPackage bare;
    bare.write("cajeta.json", "{}\n");
    bare.write("src/main/cajeta/a/B.cajeta", "package a;\n");
    std::string src = bare.path() + "/src/main/cajeta";
    EXPECT_TRUE(misplacedSkillWarnings(src, src).empty());
    EXPECT_TRUE(misplacedSkillWarnings(bare.path(), src).empty());
}
