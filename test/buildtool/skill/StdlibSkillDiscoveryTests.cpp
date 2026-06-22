// F.3 — stdlib skills are always available through discovery (spec §2.5):
// the context is seeded with the embedded stdlib archives, and the CLI
// subcommands work with NO cajeta.lock. The unit test exercises the seeding
// directly; the end-to-end test drives the real `cajeta` binary in an empty dir.

#include "cajeta/buildtool/skill/SkillCli.h"
#include "cajeta/buildtool/skill/SkillSearch.h"
#include "cajeta/buildtool/Lockfile.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

using namespace cajeta::buildtool;
using namespace cajeta::buildtool::skill;

// F.3.2 — with an empty resolved set, the context still carries stdlib skills
// and search resolves them.
TEST(StdlibSkillDiscoveryTests, contextSeededWithStdlibWhenNoPackages) {
    auto ctx = loadSkillSearchContext(
        std::vector<ResolvedPackageEntry>{},
        [](llvm::StringRef) -> std::optional<std::string> { return std::nullopt; });
    ASSERT_TRUE((bool) ctx);
    EXPECT_GE(ctx->archives.size(), 15u);

    auto results = searchSkills("cajeta.process", std::nullopt, std::nullopt, *ctx);
    ASSERT_FALSE(results.empty());
    bool sawProcess = false;
    for (const auto& r : results)
        if (r.uri.find("cja-skill://cajeta.process@") != std::string::npos)
            sawProcess = true;
    EXPECT_TRUE(sawProcess) << "search must surface an embedded stdlib skill";
}

#ifndef _WIN32

#include "cajeta/buildtool/Subprocess.h"
#include <filesystem>

namespace {
    namespace fs = std::filesystem;

    std::string cajetaExe() {
        // build/test/cajeta_test -> build/src/cajeta
        auto build = fs::canonical("/proc/self/exe").parent_path().parent_path();
        return (build / "src" / "cajeta").string();
    }

    // Run `cajeta <args...>` in `cwd`; return {exit, stdout}.
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

// F.3.1 / F.3.3 — search / list / get all return stdlib skills end-to-end with
// NO cajeta.lock in the working directory.
TEST(StdlibSkillDiscoveryTests, cliReturnsStdlibSkillsWithNoLockfile) {
    if (!fs::exists(cajetaExe())) GTEST_SKIP() << "cajeta binary not built";
    fs::path dir = fs::temp_directory_path() /
                   ("cajeta_nolock_" + std::to_string(::getpid()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    ASSERT_FALSE(fs::exists(dir / "cajeta.lock"));

    // search-skill: a library name resolves to an embedded stdlib URI.
    {
        auto [code, out] = runCajeta({"search-skill", "cajeta.process"}, dir.string());
        EXPECT_EQ(code, 0) << out;
        EXPECT_NE(out.find("cja-skill://cajeta.process@1.0/"), std::string::npos) << out;
    }
    // list-skills: enumerates stdlib entries.
    {
        auto [code, out] = runCajeta({"list-skills"}, dir.string());
        EXPECT_EQ(code, 0) << out;
        EXPECT_NE(out.find("cja-skill://cajeta."), std::string::npos) << out;
    }
    // get-skills: returns the decompressed payload for an embedded URI.
    {
        auto [code, out] = runCajeta(
            {"get-skills", "cja-skill://cajeta.process@1.0/process-overview"}, dir.string());
        EXPECT_EQ(code, 0) << out;
        EXPECT_NE(out.find("id: process-overview"), std::string::npos) << out;
    }

    fs::remove_all(dir);
}

#endif // !_WIN32
