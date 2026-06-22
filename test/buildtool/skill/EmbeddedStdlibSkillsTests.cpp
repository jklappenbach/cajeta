// F.2 — the embedded stdlib skill corpus (skill-discovery spec §2.5). The
// corpus is zstd-compressed into the compiler binary (tools/skillembed) and
// decompressed here on first access; these tests assert it yields per-library
// archives with working indexes and retrievable payloads.

#include "cajeta/buildtool/skill/EmbeddedStdlibSkills.h"
#include "cajeta/buildtool/skill/SkillSearch.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

using cajeta::buildtool::skill::embeddedStdlibSkillArchives;
using cajeta::buildtool::skill::embeddedStdlibSkillPayload;
using cajeta::buildtool::skill::kStdlibSkillVersion;
using cajeta::buildtool::skill::ResolvedSkillArchive;

namespace {
    const ResolvedSkillArchive* find(
        const std::vector<ResolvedSkillArchive>& v, const std::string& lib) {
        for (const auto& a : v)
            if (a.library == lib) return &a;
        return nullptr;
    }
}

// F.2.1 — one archive per stdlib library, version-stamped, with a working index.
TEST(EmbeddedStdlibSkillsTests, archivesCoverStdlibLibraries) {
    const auto& arcs = embeddedStdlibSkillArchives();
    // 15 top-level stdlib packages were authored (codec..wire).
    EXPECT_GE(arcs.size(), 15u);

    const ResolvedSkillArchive* proc = find(arcs, "cajeta.process");
    ASSERT_NE(proc, nullptr) << "cajeta.process archive must be embedded";
    EXPECT_EQ(proc->version, std::string(kStdlibSkillVersion));

    // The library overview skill (applies-to cajeta.process) is indexed by name.
    auto ids = proc->index.query("cajeta.process", false);
    EXPECT_NE(std::find(ids.begin(), ids.end(), "process-overview"), ids.end());
}

// F.2.1 — payloads round-trip out of the compressed corpus, decompressed.
TEST(EmbeddedStdlibSkillsTests, payloadRetrievableAndDecompressed) {
    auto p = embeddedStdlibSkillPayload("cajeta.process", "process-overview");
    ASSERT_TRUE((bool) p);
    EXPECT_NE(p->find("id: process-overview"), std::string::npos);
    EXPECT_NE(p->find("cajeta.process"), std::string::npos);

    // A non-existent (library,id) is a clean miss.
    EXPECT_FALSE((bool) embeddedStdlibSkillPayload("cajeta.process", "no-such-skill"));
}
