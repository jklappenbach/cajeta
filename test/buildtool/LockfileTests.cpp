// Regression tests for the build-tool lockfile.
// See src/cajeta/buildtool/Lockfile.h and
// plan/build-tool-plan.md Phase 2.

#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using cajeta::buildtool::checkDrift;
using cajeta::buildtool::composeLockfile;
using cajeta::buildtool::loadManifestString;
using cajeta::buildtool::Lockfile;
using cajeta::buildtool::PropertyOverrides;
using cajeta::buildtool::readLockfile;
using cajeta::buildtool::resolveProperties;
using cajeta::buildtool::sha256Hex;
using cajeta::buildtool::writeLockfile;

namespace {

    // RAII-style temp file holder for test isolation.
    struct TempFile {
        std::filesystem::path path;
        explicit TempFile(const std::string& suffix) {
            path = std::filesystem::temp_directory_path() /
                   ("cajeta-test-" +
                    std::to_string(::getpid()) + "-" +
                    std::to_string(::rand()) + "-" + suffix);
        }
        ~TempFile() {
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
        std::string str() const { return path.string(); }
    };

    cajeta::buildtool::Manifest mustLoad(const std::string& src) {
        auto m = loadManifestString(src);
        if (!m) {
            ADD_FAILURE() << "manifest load failed";
            return {};
        }
        return std::move(*m);
    }

    std::string readFile(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        std::stringstream ss; ss << in.rdbuf();
        return ss.str();
    }

} // namespace

// --- SHA-256 -------------------------------------------------------------

TEST(LockfileTests, sha256MatchesKnownVector) {
    // FIPS PUB 180-2 standard test vector: SHA-256("abc")
    EXPECT_EQ(sha256Hex("abc"),
              "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(LockfileTests, sha256OfEmptyMatchesKnownVector) {
    EXPECT_EQ(sha256Hex(""),
              "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

// --- compose + round-trip ------------------------------------------------

TEST(LockfileTests, composeFromResolvedManifest) {
    std::string src = R"({
        "details": { "name": "a.b", "version": "0.1.0" },
        "properties": { "v": "1.0" }
    })";
    auto m = mustLoad(src);
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    Lockfile lf = composeLockfile(m, src, *r, "2026-06-01T00:00:00Z");
    EXPECT_EQ(lf.lockfileVersion, 1);
    EXPECT_EQ(lf.manifestChecksum, sha256Hex(src));
    EXPECT_EQ(lf.resolvedAt, "2026-06-01T00:00:00Z");
    EXPECT_EQ(lf.properties["v"], "1.0");
    EXPECT_EQ(lf.properties["details.name"], "a.b");
}

TEST(LockfileTests, writeReadRoundTrip) {
    std::string src = R"({
        "details": { "name": "a.b", "version": "0.1.0" },
        "properties": { "x": "hello", "y": "world" }
    })";
    auto m = mustLoad(src);
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    Lockfile lf = composeLockfile(m, src, *r, "2026-06-01T00:00:00Z");

    TempFile out("lockfile.json");
    auto e = writeLockfile(out.str(), lf);
    ASSERT_FALSE((bool)e) << "write should succeed";

    auto loaded = readLockfile(out.str());
    ASSERT_TRUE((bool)loaded);
    EXPECT_EQ(loaded->lockfileVersion, lf.lockfileVersion);
    EXPECT_EQ(loaded->manifestChecksum, lf.manifestChecksum);
    EXPECT_EQ(loaded->resolvedAt, lf.resolvedAt);
    EXPECT_EQ(loaded->properties.size(), lf.properties.size());
    EXPECT_EQ(loaded->properties["x"], "hello");
    EXPECT_EQ(loaded->properties["y"], "world");
}

TEST(LockfileTests, writeProducesByteIdenticalOutputForSameInputs) {
    std::string src = R"({
        "details": { "name": "a.b", "version": "0.1.0" },
        "properties": { "x": "hello" }
    })";
    auto m = mustLoad(src);
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    Lockfile lf1 = composeLockfile(m, src, *r, "2026-06-01T00:00:00Z");
    Lockfile lf2 = composeLockfile(m, src, *r, "2026-06-01T00:00:00Z");

    TempFile o1("lf1.json"), o2("lf2.json");
    ASSERT_FALSE((bool)writeLockfile(o1.str(), lf1));
    ASSERT_FALSE((bool)writeLockfile(o2.str(), lf2));
    EXPECT_EQ(readFile(o1.str()), readFile(o2.str()));
}

TEST(LockfileTests, writeIsStableAcrossPropertyInsertionOrder) {
    // Even if properties were inserted in different orders into the
    // map, the on-disk JSON sorts them — so two lockfiles built from
    // the same logical resolved set are byte-identical.
    std::string src = R"({
        "details": { "name": "a.b", "version": "0.1.0" },
        "properties": { "b": "2", "a": "1", "c": "3" }
    })";
    auto m = mustLoad(src);
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    Lockfile lf = composeLockfile(m, src, *r, "2026-06-01T00:00:00Z");

    TempFile out("lockfile.json");
    ASSERT_FALSE((bool)writeLockfile(out.str(), lf));
    auto text = readFile(out.str());
    // Properties should appear in lexicographic order in the file.
    auto posA = text.find("\"a\"");
    auto posB = text.find("\"b\"");
    auto posC = text.find("\"c\"");
    EXPECT_NE(posA, std::string::npos);
    EXPECT_LT(posA, posB);
    EXPECT_LT(posB, posC);
}

// --- drift detection -----------------------------------------------------

TEST(LockfileTests, driftReportsNoChangeOnUnmodifiedSource) {
    std::string src = R"({"details":{"name":"a.b","version":"0.1"}})";
    auto m = mustLoad(src);
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    Lockfile lf = composeLockfile(m, src, *r, "2026-06-01T00:00:00Z");
    auto rep = checkDrift(lf, src);
    EXPECT_FALSE(rep.changed);
    EXPECT_EQ(rep.oldChecksum, rep.newChecksum);
}

TEST(LockfileTests, driftReportsChangeOnModifiedSource) {
    std::string src1 = R"({"details":{"name":"a.b","version":"0.1"}})";
    std::string src2 = R"({"details":{"name":"a.b","version":"0.2"}})";  // patch bump
    auto m = mustLoad(src1);
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    Lockfile lf = composeLockfile(m, src1, *r, "2026-06-01T00:00:00Z");
    auto rep = checkDrift(lf, src2);
    EXPECT_TRUE(rep.changed);
    EXPECT_NE(rep.oldChecksum, rep.newChecksum);
}

TEST(LockfileTests, driftReportsChangeOnWhitespaceChange) {
    // Drift is byte-exact: even reformatting the manifest triggers
    // it. The right resolution for whitespace-only changes is "run
    // build, accept the new checksum"; this test pins the
    // strict-bytewise behavior so we don't accidentally start
    // normalizing.
    std::string src1 = R"({"details":{"name":"a.b","version":"0.1"}})";
    std::string src2 = R"({
  "details": { "name": "a.b", "version": "0.1" }
})";
    auto m = mustLoad(src1);
    auto r = resolveProperties(m);
    ASSERT_TRUE((bool)r);
    Lockfile lf = composeLockfile(m, src1, *r, "2026-06-01T00:00:00Z");
    auto rep = checkDrift(lf, src2);
    EXPECT_TRUE(rep.changed);
}

// --- error cases ---------------------------------------------------------

TEST(LockfileTests, readErrorsOnMissingFile) {
    auto r = readLockfile("/no/such/path/to/lockfile.lock");
    EXPECT_FALSE((bool)r);
    if (!r) consumeError(r.takeError());
}

TEST(LockfileTests, readErrorsOnMalformedJson) {
    TempFile out("bad.lock");
    {
        std::ofstream o(out.str());
        o << "{ not even close";
    }
    auto r = readLockfile(out.str());
    EXPECT_FALSE((bool)r);
    if (!r) consumeError(r.takeError());
}

TEST(LockfileTests, readErrorsOnMissingRequiredField) {
    TempFile out("incomplete.lock");
    {
        std::ofstream o(out.str());
        o << R"({"resolved-at": "2026-06-01T00:00:00Z"})";
    }
    auto r = readLockfile(out.str());
    EXPECT_FALSE((bool)r);
    if (!r) consumeError(r.takeError());
}

// --- timestamp -----------------------------------------------------------

TEST(LockfileTests, nowIsoUtcMatchesExpectedShape) {
    auto t = cajeta::buildtool::nowIsoUtc();
    // ISO 8601 UTC: YYYY-MM-DDTHH:MM:SSZ — 20 chars exactly.
    EXPECT_EQ(t.size(), 20u);
    EXPECT_EQ(t[4], '-');
    EXPECT_EQ(t[7], '-');
    EXPECT_EQ(t[10], 'T');
    EXPECT_EQ(t[13], ':');
    EXPECT_EQ(t[16], ':');
    EXPECT_EQ(t[19], 'Z');
}
