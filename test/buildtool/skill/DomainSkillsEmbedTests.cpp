// compiler-mcp Unit 1 — runtime/skills/<domain> corpus extension (spec §4).
// Domain directories embed as pseudo-libraries `cajeta.<domain>`; the relocated
// driver skills prove the mechanism as `cajeta.toolchain` and stay reachable
// through search/list/get with no project or lockfile.

#include "cajeta/buildtool/skill/EmbeddedStdlibSkills.h"
#include "cajeta/buildtool/skill/SkillCli.h"
#include "cajeta/buildtool/skill/SkillSearch.h"
#include "cajeta/buildtool/Lockfile.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

using namespace cajeta::buildtool;
using namespace cajeta::buildtool::skill;

namespace {
    const ResolvedSkillArchive* find(
        const std::vector<ResolvedSkillArchive>& v, const std::string& lib) {
        for (const auto& a : v)
            if (a.library == lib) return &a;
        return nullptr;
    }
}

// 1.1.1 — a runtime/skills/<domain> directory embeds as library
// `cajeta.<domain>`, version-stamped, indexed, payloads retrievable.
TEST(DomainSkillsEmbedTests, toolchainDomainEmbedsAsPseudoLibrary) {
    const auto& arcs = embeddedStdlibSkillArchives();
    const ResolvedSkillArchive* tc = find(arcs, "cajeta.toolchain");
    ASSERT_NE(tc, nullptr) << "cajeta.toolchain archive must be embedded";
    EXPECT_EQ(tc->version, std::string(kStdlibSkillVersion));

    auto ids = tc->index.query("cajeta.toolchain", false);
    EXPECT_NE(std::find(ids.begin(), ids.end(), "cajeta-driver-overview"),
              ids.end());

    auto p = embeddedStdlibSkillPayload("cajeta.toolchain", "cajeta-driver-overview");
    ASSERT_TRUE((bool) p);
    EXPECT_NE(p->find("id: cajeta-driver-overview"), std::string::npos);
}

// 1.1.2 — the relocated driver skills resolve through search with an empty
// resolved set (embedded corpus only), and the legacy `cajeta-driver` binding
// still matches (alias preserved).
TEST(DomainSkillsEmbedTests, searchResolvesDriverSkillsFromEmbeddedCorpus) {
    auto ctx = loadSkillSearchContext(
        std::vector<ResolvedPackageEntry>{},
        [](llvm::StringRef) -> std::optional<std::string> { return std::nullopt; });
    ASSERT_TRUE((bool) ctx);

    for (const char* query : {"cajeta-driver", "cajeta.toolchain"}) {
        auto results = searchSkills(query, std::nullopt, std::nullopt, *ctx);
        bool sawToolchain = false;
        for (const auto& r : results)
            if (r.uri.find("cja-skill://cajeta.toolchain@") != std::string::npos)
                sawToolchain = true;
        EXPECT_TRUE(sawToolchain) << "query '" << query
                                  << "' must surface a cajeta.toolchain skill";
    }
}

// 1.1.3 — list scoped to the domain subtree enumerates all five driver skills.
TEST(DomainSkillsEmbedTests, listScopedToToolchainEnumeratesDriverSkills) {
    auto ctx = loadSkillSearchContext(
        std::vector<ResolvedPackageEntry>{},
        [](llvm::StringRef) -> std::optional<std::string> { return std::nullopt; });
    ASSERT_TRUE((bool) ctx);

    auto entries = listSkills(std::string("cajeta/toolchain"), std::nullopt,
                              std::nullopt, *ctx);
    auto has = [&](const std::string& id) {
        for (const auto& e : entries)
            if (e.uri.size() >= id.size() &&
                e.uri.compare(e.uri.size() - id.size(), id.size(), id) == 0)
                return true;
        return false;
    };
    EXPECT_GE(entries.size(), 5u);
    for (const char* id : {"cajeta-driver-overview", "cajeta-driver-compile",
                           "cajeta-driver-jit-run", "cajeta-driver-tasks",
                           "cajeta-driver-skill-discovery"}) {
        EXPECT_TRUE(has(id)) << id << " must be listed under cajeta/toolchain";
    }
}

#ifndef _WIN32

#include "cajeta/buildtool/Subprocess.h"
#include <filesystem>
#include <unistd.h>

namespace {
    namespace fs = std::filesystem;

    std::string cajetaExe() {
        auto build = fs::canonical("/proc/self/exe").parent_path().parent_path();
        return (build / "src" / "cajeta").string();
    }

    std::pair<int, std::string> runCajeta(const std::vector<std::string>& args,
                                          const std::string& cwd) {
        std::vector<std::string> argv = {cajetaExe()};
        for (auto& a : args) argv.push_back(a);
        std::string out, err;
        SubprocessOptions opt;
        opt.argv = argv;
        opt.cwd = &cwd;
        opt.outData = &out;
        opt.errData = &err;
        auto r = runSubprocess(opt);
        EXPECT_TRUE(r.launched) << "spawn failed: " << err;
        return {r.code(), out};
    }
}

// 1.3.1 — the rebuilt binary answers domain-skill queries from a directory
// with no project, via the CLI the MCP server will front.
TEST(DomainSkillsEmbedTests, cliServesToolchainSkillsWithNoProject) {
    if (!fs::exists(cajetaExe())) GTEST_SKIP() << "cajeta binary not built";
    fs::path dir = fs::temp_directory_path() /
                   ("cajeta_domain_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);

    {
        // Typo'd canonical name — fuzzy match must still resolve the domain skill.
        auto [code, out] =
            runCajeta({"search-skill", "cajeta/toolchan/jit-run"}, dir.string());
        EXPECT_EQ(code, 0) << out;
        EXPECT_NE(out.find("cja-skill://cajeta.toolchain@1.0/cajeta-driver-jit-run"),
                  std::string::npos) << out;
    }
    {
        auto [code, out] = runCajeta(
            {"get-skills",
             "cja-skill://cajeta.toolchain@1.0/cajeta-driver-overview"},
            dir.string());
        EXPECT_EQ(code, 0) << out;
        EXPECT_NE(out.find("id: cajeta-driver-overview"), std::string::npos) << out;
    }

    fs::remove_all(dir);
}

#endif // !_WIN32

// ---- compiler-mcp Unit 4+ — the minimal expert skill catalog (spec §5-§7).
// Extended as catalog batches land; each id must resolve by payload and be
// reachable through search via its primary applies-to binding.

struct CatalogEntry {
    const char* library;
    const char* id;
    const char* binding; // primary applies-to name
};

static const CatalogEntry kCatalog[] = {
    // Unit 4 — language batch 1
    {"cajeta.language", "language-overview", "cajeta.language"},
    {"cajeta.language", "language-types-and-allocation", "cajeta/language/types"},
    {"cajeta.language", "language-ownership", "cajeta/language/ownership"},
    // Unit 5 — language batch 2
    {"cajeta.language", "language-classes", "cajeta/language/classes"},
    {"cajeta.language", "language-classes", "cajeta/language/operators"},
    {"cajeta.language", "language-classes", "cajeta/language/inheritance"},
    {"cajeta.language", "language-templates", "cajeta/language/templates"},
    {"cajeta.language", "language-lambdas", "cajeta/language/lambdas"},
};

TEST(CatalogSkillsTests, catalogIdsResolveAndBindingsSearchable) {
    auto ctx = loadSkillSearchContext(
        std::vector<ResolvedPackageEntry>{},
        [](llvm::StringRef) -> std::optional<std::string> { return std::nullopt; });
    ASSERT_TRUE((bool) ctx);

    for (const auto& e : kCatalog) {
        auto p = embeddedStdlibSkillPayload(e.library, e.id);
        ASSERT_TRUE((bool) p) << e.id << " must be embedded in " << e.library;
        EXPECT_NE(p->find(std::string("id: ") + e.id), std::string::npos) << e.id;

        auto results = searchSkills(e.binding, std::nullopt, std::nullopt, *ctx);
        bool found = false;
        for (const auto& r : results)
            if (r.uri.find(std::string("/") + e.id) != std::string::npos)
                found = true;
        EXPECT_TRUE(found) << e.binding << " must surface " << e.id;
    }
}
