// Tests for the cja-skill:// URI scheme + lockfile resolver.
// See src/cajeta/buildtool/skill/SkillUri.h and
// docs/specs/skill-discovery-spec.md §2.2 (plan unit D.4).

#include "cajeta/buildtool/skill/SkillUri.h"

#include <gtest/gtest.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <optional>
#include <string>

using cajeta::buildtool::ResolvedPackageEntry;
using cajeta::buildtool::skill::resolveSkillArchive;
using cajeta::buildtool::skill::SkillUri;

namespace {

    template <class T> T unwrap(llvm::Expected<T> e) {
        EXPECT_TRUE((bool)e);
        if (!e) {
            consumeError(e.takeError());
            return T{};
        }
        return std::move(*e);
    }

    std::string err(llvm::Error e) {
        std::string s;
        llvm::raw_string_ostream os(s);
        os << e;
        return s;
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

// D.4.1 — parse/format round-trips.
TEST(SkillUriTests, parseFormatRoundTrip) {
    auto u = unwrap(SkillUri::parse("cja-skill://cajeta.io@1.4.2/file-open"));
    EXPECT_EQ(u.library, "cajeta.io");
    EXPECT_EQ(u.version, "1.4.2");
    EXPECT_EQ(u.skillId, "file-open");
    EXPECT_EQ(u.format(), "cja-skill://cajeta.io@1.4.2/file-open");
    EXPECT_EQ(unwrap(SkillUri::parse(u.format())), u);
}

// D.4.1 — malformed URIs are rejected.
TEST(SkillUriTests, rejectsMalformed) {
    EXPECT_FALSE((bool)SkillUri::parse("http://cajeta.io@1/x"));   // wrong scheme
    EXPECT_FALSE((bool)SkillUri::parse("cja-skill://cajeta.io/x")); // no @version
    EXPECT_FALSE((bool)SkillUri::parse("cja-skill://cajeta.io@/x")); // empty version
    EXPECT_FALSE((bool)SkillUri::parse("cja-skill://@1.4.2/x"));     // empty library
    EXPECT_FALSE((bool)SkillUri::parse("cja-skill://cajeta.io@1.4.2/")); // empty id
    EXPECT_FALSE((bool)SkillUri::parse("cja-skill://cajeta.io@1.4.2"));  // no id sep

    // Errors mention the offending aspect for diagnostics.
    auto e = SkillUri::parse("cja-skill://cajeta.io/file-open");
    ASSERT_FALSE((bool)e);
    EXPECT_NE(err(e.takeError()).find("version"), std::string::npos);
}

// D.4.1 — resolve <library>@<version> to a local .cja via the lockfile + cache.
TEST(SkillUriTests, resolvesAgainstLockfile) {
    std::vector<ResolvedPackageEntry> pkgs = {
        pkg("cajeta.io", "1.4.2", "sha256:aaa"),
        pkg("cajeta.net", "2.0.0", "sha256:bbb"),
    };
    auto lookup = [](llvm::StringRef sum) -> std::optional<std::string> {
        if (sum == "sha256:aaa") return std::string("/cache/aaa.cja");
        if (sum == "sha256:bbb") return std::string("/cache/bbb.cja");
        return std::nullopt;
    };
    auto uri = unwrap(SkillUri::parse("cja-skill://cajeta.io@1.4.2/file-open"));
    EXPECT_EQ(unwrap(resolveSkillArchive(uri, pkgs, lookup)), "/cache/aaa.cja");
}

// D.4.1 — an unresolved coordinate is a clear error.
TEST(SkillUriTests, unresolvedCoordinateError) {
    std::vector<ResolvedPackageEntry> pkgs = {
        pkg("cajeta.io", "1.4.2", "sha256:aaa"),
    };
    auto lookup = [](llvm::StringRef) -> std::optional<std::string> {
        return std::string("/cache/aaa.cja");
    };
    auto uri = unwrap(SkillUri::parse("cja-skill://cajeta.io@9.9.9/x"));
    auto r = resolveSkillArchive(uri, pkgs, lookup);
    ASSERT_FALSE((bool)r);
    std::string m = err(r.takeError());
    EXPECT_NE(m.find("cajeta.io"), std::string::npos);
    EXPECT_NE(m.find("9.9.9"), std::string::npos);
}

// D.4.1 — resolved in lockfile but absent from the local cache is an error.
TEST(SkillUriTests, cacheMissError) {
    std::vector<ResolvedPackageEntry> pkgs = {
        pkg("cajeta.io", "1.4.2", "sha256:aaa"),
    };
    auto lookup = [](llvm::StringRef) -> std::optional<std::string> {
        return std::nullopt; // not cached
    };
    auto uri = unwrap(SkillUri::parse("cja-skill://cajeta.io@1.4.2/x"));
    auto r = resolveSkillArchive(uri, pkgs, lookup);
    ASSERT_FALSE((bool)r);
    EXPECT_NE(err(r.takeError()).find("cache"), std::string::npos);
}

// D.4.3 — exact version pinning: a different resolved version does not match.
TEST(SkillUriTests, exactVersionPinning) {
    std::vector<ResolvedPackageEntry> pkgs = {
        pkg("cajeta.io", "1.5.0", "sha256:aaa"),
    };
    auto lookup = [](llvm::StringRef) -> std::optional<std::string> {
        return std::string("/cache/aaa.cja");
    };
    auto uri = unwrap(SkillUri::parse("cja-skill://cajeta.io@1.4.2/x"));
    EXPECT_FALSE((bool)resolveSkillArchive(uri, pkgs, lookup)); // 1.4.2 != 1.5.0
}
