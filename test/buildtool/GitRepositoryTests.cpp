// Regression tests for the Phase 6c Git repository driver. These
// guard:
//   - settings.repositories[].{ref,tag,branch,rev,subdir} parsing.
//   - The driver's clone → checkout → cajeta.json read cycle against
//     a local file:// URL (no network).
//   - listVersions / fetch / fetchManifestJson honor the declared
//     `details.name` + `details.version`.
//   - Missing pre-built .cja under the checkout is reported clearly
//     (the v1 limitation — recursive `cajeta build` not yet wired).
//
// The fixture creates a tiny upstream repository on disk: `git init`,
// commits a `cajeta.json` (and optionally a pre-built artifact), and
// hands the driver a `file://` URL. Tests skip gracefully if the
// `git` binary isn't on PATH.

#include "cajeta/buildtool/Dependency.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/repo/GitRepository.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::GitRepository;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::parseRepositories;
using cajeta::buildtool::RepositorySpec;

namespace {

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    bool gitOnPath() {
        return std::system("git --version >/dev/null 2>&1") == 0;
    }

    std::filesystem::path makeTempDir(const std::string& tag) {
        auto p = std::filesystem::temp_directory_path() /
                 ("cajeta-git-test-" + tag + "-" +
                  std::to_string(::getpid()) + "-" +
                  std::to_string(::rand()));
        std::filesystem::create_directories(p);
        return p;
    }

    // Set up a brand-new local git repo with one commit. Returns
    // `(repoDir, fileUrl)`. The repo's working tree contains the
    // files in `files` (relative-path → contents) at the time of
    // the initial commit. If `subdir` is non-empty, all entries go
    // under that subdir.
    struct UpstreamRepo {
        std::filesystem::path dir;
        std::string url;
        std::string tag;  // tag created at the initial commit
    };
    UpstreamRepo makeUpstream(
        const std::string& tag,
        const std::vector<std::pair<std::string, std::string>>& files,
        const std::string& subdir = "") {
        auto dir = makeTempDir("upstream");
        // git init + identity config (CI runners often have neither).
        // All git output silenced so test logs stay clean.
        auto run = [&](const std::string& cmd) {
            std::string full = "cd " + dir.string() + " && " +
                               cmd + " >/dev/null 2>&1";
            EXPECT_EQ(0, std::system(full.c_str())) << full;
        };
        run("git init -q -b main");
        run("git config user.email test@cajeta");
        run("git config user.name CajetaTest");

        for (const auto& [rel, contents] : files) {
            auto full = dir;
            if (!subdir.empty()) full /= subdir;
            full /= rel;
            std::filesystem::create_directories(full.parent_path());
            std::ofstream o(full, std::ios::binary);
            o << contents;
        }
        run("git add -A");
        run("git commit -q -m initial");
        run("git tag " + tag);

        UpstreamRepo r;
        r.dir = dir;
        r.url = "file://" + dir.string();
        r.tag = tag;
        return r;
    }

    std::string manifestJson(const std::string& name,
                             const std::string& version) {
        std::ostringstream o;
        o << "{\"details\":{\"name\":\"" << name
          << "\",\"version\":\"" << version << "\"}}";
        return o.str();
    }

} // namespace

// ─── parser ────────────────────────────────────────────────────────

TEST(GitRepositoryTests, parserAcceptsTagTagBranchRev) {
    auto src = R"({
        "details": { "name": "p", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "by-tag",    "type": "git",
                  "url": "https://example.com/x", "tag": "v0.1.0" },
                { "name": "by-branch", "type": "git",
                  "url": "https://example.com/x", "branch": "main" },
                { "name": "by-rev",    "type": "git",
                  "url": "https://example.com/x",
                  "rev": "abc1234", "subdir": "pkg/core" }
            ]
        }
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE(static_cast<bool>(m)) << errorText(m.takeError());
    auto repos = parseRepositories(*m);
    ASSERT_TRUE(static_cast<bool>(repos)) << errorText(repos.takeError());
    ASSERT_EQ(repos->size(), 3u);
    EXPECT_EQ((*repos)[0].gitRef, "v0.1.0");
    EXPECT_EQ((*repos)[1].gitRef, "main");
    EXPECT_EQ((*repos)[2].gitRef, "abc1234");
    EXPECT_EQ((*repos)[2].gitSubdir, "pkg/core");
}

TEST(GitRepositoryTests, parserRejectsGitWithoutRef) {
    auto src = R"({
        "details": { "name": "p", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "noref", "type": "git",
                  "url": "https://example.com/x" }
            ]
        }
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE(static_cast<bool>(m)) << errorText(m.takeError());
    auto repos = parseRepositories(*m);
    ASSERT_FALSE(static_cast<bool>(repos));
    EXPECT_NE(errorText(repos.takeError()).find("requires one of 'ref'"),
              std::string::npos);
}

TEST(GitRepositoryTests, parserRejectsGitWithoutUrl) {
    auto src = R"({
        "details": { "name": "p", "version": "0.1.0" },
        "settings": {
            "repositories": [
                { "name": "nourl", "type": "git", "ref": "main" }
            ]
        }
    })";
    auto m = loadManifestString(src);
    ASSERT_TRUE(static_cast<bool>(m)) << errorText(m.takeError());
    auto repos = parseRepositories(*m);
    ASSERT_FALSE(static_cast<bool>(repos));
    EXPECT_NE(errorText(repos.takeError()).find("requires 'url'"),
              std::string::npos);
}

// ─── driver: clone + read manifest ────────────────────────────────

TEST(GitRepositoryTests, listVersionsReturnsDeclaredVersionOnMatch) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v1.2.3", {
        {"cajeta.json", manifestJson("acme.lib", "1.2.3")},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v1.2.3", "", stage);
    auto vs = r.listVersions("acme.lib");
    ASSERT_TRUE(static_cast<bool>(vs)) << errorText(vs.takeError());
    ASSERT_EQ(vs->size(), 1u);
    EXPECT_EQ((*vs)[0], "1.2.3");
}

TEST(GitRepositoryTests, listVersionsReturnsEmptyOnNameMismatch) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v1.0.0", {
        {"cajeta.json", manifestJson("acme.lib", "1.0.0")},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v1.0.0", "", stage);
    auto vs = r.listVersions("not.acme.lib");
    ASSERT_TRUE(static_cast<bool>(vs)) << errorText(vs.takeError());
    EXPECT_EQ(vs->size(), 0u);
}

TEST(GitRepositoryTests, fetchManifestJsonReturnsBytes) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v0.9.1", {
        {"cajeta.json", manifestJson("acme.util", "0.9.1")},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v0.9.1", "", stage);
    auto mj = r.fetchManifestJson("acme.util", "0.9.1");
    ASSERT_TRUE(static_cast<bool>(mj)) << errorText(mj.takeError());
    ASSERT_TRUE(mj->has_value());
    EXPECT_NE((*mj)->find("\"acme.util\""), std::string::npos);
}

TEST(GitRepositoryTests, fetchManifestJsonNulloptOnNameOrVersionMismatch) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v1.0.0", {
        {"cajeta.json", manifestJson("acme.lib", "1.0.0")},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v1.0.0", "", stage);
    auto a = r.fetchManifestJson("acme.lib", "9.9.9");
    ASSERT_TRUE(static_cast<bool>(a)) << errorText(a.takeError());
    EXPECT_FALSE(a->has_value());

    auto b = r.fetchManifestJson("other.pkg", "1.0.0");
    ASSERT_TRUE(static_cast<bool>(b)) << errorText(b.takeError());
    EXPECT_FALSE(b->has_value());
}

// ─── driver: fetch artifact ────────────────────────────────────────

TEST(GitRepositoryTests, fetchReturnsArtifactPathWhenPrebuilt) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v1.0.0", {
        {"cajeta.json", manifestJson("acme.lib", "1.0.0")},
        {"build/archive/acme.lib-1.0.0.cja", "stub-bytes"},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v1.0.0", "", stage);
    auto p = r.fetch("acme.lib", "1.0.0");
    ASSERT_TRUE(static_cast<bool>(p)) << errorText(p.takeError());
    EXPECT_NE(p->find("acme.lib-1.0.0.cja"), std::string::npos);
    EXPECT_TRUE(std::filesystem::is_regular_file(*p));
}

TEST(GitRepositoryTests, fetchErrorsWhenArtifactMissing) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v1.0.0", {
        {"cajeta.json", manifestJson("acme.lib", "1.0.0")},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v1.0.0", "", stage);
    auto p = r.fetch("acme.lib", "1.0.0");
    ASSERT_FALSE(static_cast<bool>(p));
    auto msg = errorText(p.takeError());
    EXPECT_NE(msg.find("expected pre-built artifact"), std::string::npos);
}

TEST(GitRepositoryTests, fetchErrorsOnVersionMismatch) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v1.0.0", {
        {"cajeta.json", manifestJson("acme.lib", "1.0.0")},
        {"build/archive/acme.lib-1.0.0.cja", "stub"},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v1.0.0", "", stage);
    auto p = r.fetch("acme.lib", "2.0.0");
    ASSERT_FALSE(static_cast<bool>(p));
    EXPECT_NE(errorText(p.takeError()).find("does not carry"),
              std::string::npos);
}

// ─── driver: subdir layout ─────────────────────────────────────────

TEST(GitRepositoryTests, subdirLocatesNestedManifest) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v0.5.0", {
        {"cajeta.json", manifestJson("acme.nested", "0.5.0")},
        {"build/archive/acme.nested-0.5.0.cja", "stub"},
    }, "pkg/core");
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v0.5.0", "pkg/core", stage);
    auto vs = r.listVersions("acme.nested");
    ASSERT_TRUE(static_cast<bool>(vs)) << errorText(vs.takeError());
    ASSERT_EQ(vs->size(), 1u);
    EXPECT_EQ((*vs)[0], "0.5.0");

    auto p = r.fetch("acme.nested", "0.5.0");
    ASSERT_TRUE(static_cast<bool>(p)) << errorText(p.takeError());
    EXPECT_NE(p->find("pkg/core/build/archive"), std::string::npos);
}

// ─── driver: clone is cached ───────────────────────────────────────

TEST(GitRepositoryTests, secondListReusesClone) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v0.1.0", {
        {"cajeta.json", manifestJson("acme.lib", "0.1.0")},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "v0.1.0", "", stage);
    auto v1 = r.listVersions("acme.lib");
    ASSERT_TRUE(static_cast<bool>(v1)) << errorText(v1.takeError());

    // Remove the upstream entirely; second call must still succeed
    // because the clone has already been materialised locally.
    std::filesystem::remove_all(up.dir);
    auto v2 = r.listVersions("acme.lib");
    ASSERT_TRUE(static_cast<bool>(v2)) << errorText(v2.takeError());
    EXPECT_EQ(v1->size(), v2->size());
}

// ─── driver: bad ref + bad URL ─────────────────────────────────────

TEST(GitRepositoryTests, cloneFailureSurfacesClearError) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto stage = makeTempDir("stage").string();
    GitRepository r("bad-url", "file:///nonexistent/path/abc.git",
                    "main", "", stage);
    auto vs = r.listVersions("any.pkg");
    ASSERT_FALSE(static_cast<bool>(vs));
    EXPECT_NE(errorText(vs.takeError()).find("clone failed"),
              std::string::npos);
}

TEST(GitRepositoryTests, badRefSurfacesClearError) {
    if (!gitOnPath()) GTEST_SKIP() << "git not on PATH";

    auto up = makeUpstream("v1.0.0", {
        {"cajeta.json", manifestJson("acme.lib", "1.0.0")},
    });
    auto stage = makeTempDir("stage").string();

    GitRepository r("test-git", up.url, "ref-does-not-exist", "", stage);
    auto vs = r.listVersions("acme.lib");
    ASSERT_FALSE(static_cast<bool>(vs));
    EXPECT_NE(errorText(vs.takeError()).find("checkout"),
              std::string::npos);
}
