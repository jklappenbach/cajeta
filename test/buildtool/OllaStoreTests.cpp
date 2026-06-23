// Unit 1 (olla-local-repo plan) — the `~/.olla/` local repository store.
//
// Pins the store's contract (spec 2):
//   - Root resolution: $OLLA_HOME wins; else <home>/.olla; a test
//     home-override stands in for $HOME (mirrors ArtifactCache).      [1.1.1]
//   - write→read round-trip lands at the filesystem-repo layout path
//     and creates the <name>/<version>/ subtree on demand.            [1.1.2]
//   - Writes are atomic: temp + rename, no .tmp residue, overwrite
//     replaces bytes in place.                                        [1.1.3]
//   - versions.json is updated and idempotent.                        [1.1.4]
//   - The store reads a pre-staged filesystem-repo tree unchanged
//     (layout parity with FilesystemRepository).                      [1.3.1]

#include "cajeta/buildtool/OllaStore.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"

#include <gtest/gtest.h>

#include "../PortableEnv.h"
#include <llvm/Support/Error.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

using cajeta::buildtool::FilesystemRepository;
using cajeta::buildtool::OllaStore;

namespace {

    namespace fs = std::filesystem;

    std::string errorText(llvm::Error&& err) {
        std::string out;
        llvm::raw_string_ostream os(out);
        os << err;
        consumeError(std::move(err));
        return out;
    }

    fs::path tempDir(const std::string& tag) {
        auto p = fs::temp_directory_path() /
                 ("cajeta-olla-" + tag + "-" + std::to_string(::getpid()) +
                  "-" + std::to_string(::rand()));
        fs::create_directories(p);
        return p;
    }

    void writeFile(const fs::path& p, const std::string& bytes) {
        fs::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << bytes;
    }

    std::string readFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }

    // RAII save/restore of an env var so root-resolution tests don't
    // leak $OLLA_HOME into sibling tests in the same process.
    struct EnvGuard {
        std::string key;
        bool had;
        std::string old;
        explicit EnvGuard(std::string k) : key(std::move(k)) {
            const char* v = ::getenv(key.c_str());
            had = v != nullptr;
            if (had) old = v;
        }
        void set(const std::string& v) { ::setenv(key.c_str(), v.c_str(), 1); }
        void unset() { ::unsetenv(key.c_str()); }
        ~EnvGuard() {
            if (had) ::setenv(key.c_str(), old.c_str(), 1);
            else ::unsetenv(key.c_str());
        }
    };

} // namespace

// 1.1.1 — $OLLA_HOME wins over the home-derived default.
TEST(OllaStoreTest, RootHonorsOllaHomeEnv) {
    EnvGuard og("OLLA_HOME");
    auto explicitRoot = tempDir("ollahome");
    og.set(explicitRoot.string());
    EXPECT_EQ(OllaStore::resolveRoot(/*homeOverride=*/std::string("/nope")),
              explicitRoot.string());
}

// 1.1.1 — without $OLLA_HOME, the root is <home>/.olla; the override
// stands in for $HOME.
TEST(OllaStoreTest, RootDefaultsToHomeDotOlla) {
    EnvGuard og("OLLA_HOME");
    og.unset();
    auto home = tempDir("home");
    EXPECT_EQ(OllaStore::resolveRoot(home.string()),
              (home / ".olla").string());
}

// 1.1.2 — write→read round-trip, subtree created on demand.
TEST(OllaStoreTest, WriteReadRoundTrip) {
    auto root = tempDir("rt");
    OllaStore store(root.string());

    auto src = tempDir("src") / "artifact.cja";
    writeFile(src, "GZIP-BYTES-v1");

    auto written = store.write("dev.cajeta.codec", "0.5.0", src.string(),
                               /*manifestPath=*/std::nullopt);
    ASSERT_TRUE(static_cast<bool>(written))
        << errorText(written.takeError());

    auto expected = (root / "dev.cajeta.codec" / "0.5.0" /
                     "dev.cajeta.codec-0.5.0.cja").string();
    EXPECT_EQ(*written, expected);
    EXPECT_TRUE(fs::exists(expected));

    auto found = store.read("dev.cajeta.codec", "0.5.0");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(readFile(*found), "GZIP-BYTES-v1");

    EXPECT_FALSE(store.read("dev.cajeta.codec", "9.9.9").has_value());
}

// 1.1.3 — atomic: overwrite replaces bytes in place and no .tmp file
// is left behind.
TEST(OllaStoreTest, OverwriteIsAtomicNoTempResidue) {
    auto root = tempDir("atomic");
    OllaStore store(root.string());

    auto src1 = tempDir("s1") / "a.cja";
    writeFile(src1, "v1");
    ASSERT_TRUE(static_cast<bool>(
        store.write("pkg", "1.0.0", src1.string(), std::nullopt)));

    auto src2 = tempDir("s2") / "a.cja";
    writeFile(src2, "v2-longer");
    auto w2 = store.write("pkg", "1.0.0", src2.string(), std::nullopt);
    ASSERT_TRUE(static_cast<bool>(w2)) << errorText(w2.takeError());

    EXPECT_EQ(readFile(*w2), "v2-longer");

    // No leftover temp files in the version directory.
    auto verDir = root / "pkg" / "1.0.0";
    for (const auto& e : fs::directory_iterator(verDir)) {
        EXPECT_EQ(e.path().extension(), ".cja")
            << "unexpected residue: " << e.path();
    }
}

// 1.1.4 — versions.json is maintained and idempotent.
TEST(OllaStoreTest, VersionsJsonMaintainedIdempotent) {
    auto root = tempDir("versions");
    OllaStore store(root.string());

    auto src = tempDir("vsrc") / "a.cja";
    writeFile(src, "x");

    ASSERT_TRUE(static_cast<bool>(
        store.write("pkg", "0.5.0", src.string(), std::nullopt)));
    // Re-write the same version → still a single entry.
    ASSERT_TRUE(static_cast<bool>(
        store.write("pkg", "0.5.0", src.string(), std::nullopt)));
    ASSERT_TRUE(static_cast<bool>(
        store.write("pkg", "0.6.0", src.string(), std::nullopt)));

    auto versionsJson = readFile((root / "pkg" / "versions.json").string());
    // Each version appears exactly once.
    auto count = [&](const std::string& v) {
        size_t n = 0, pos = 0;
        std::string needle = "\"" + v + "\"";
        while ((pos = versionsJson.find(needle, pos)) != std::string::npos) {
            ++n;
            pos += needle.size();
        }
        return n;
    };
    EXPECT_EQ(count("0.5.0"), 1u);
    EXPECT_EQ(count("0.6.0"), 1u);
}

// 1.3.1 — a pre-staged filesystem-repo tree is read identically by a
// FilesystemRepository pointed at the same root (layout parity).
TEST(OllaStoreTest, LayoutParityWithFilesystemRepository) {
    auto root = tempDir("parity");
    OllaStore store(root.string());

    auto src = tempDir("psrc") / "a.cja";
    writeFile(src, "ARTIFACT");
    ASSERT_TRUE(static_cast<bool>(
        store.write("cajeta.math", "1.2.4", src.string(), std::nullopt)));

    FilesystemRepository repo("olla", root.string());
    auto fetched = repo.fetch("cajeta.math", "1.2.4");
    ASSERT_TRUE(static_cast<bool>(fetched)) << errorText(fetched.takeError());
    EXPECT_EQ(readFile(*fetched), "ARTIFACT");
}
