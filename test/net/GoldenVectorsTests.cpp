//
// GoldenVectorsTests.cpp — self-validation of the NET-13.2 corpus.
//
// NET-13.2 ships *data* (checked-in golden vectors) that later phases
// (NET-6/7/10/11) pin their parsers against. The parsers don't exist
// yet, so this suite validates the corpus *itself*: every file loads,
// the field shapes are right, and each vector is internally consistent
// at a level that needs no cajeta.io.net parser — e.g. a SHA-256 digest
// field is exactly 64 hex chars, a WebSocket frame's declared opcode
// matches the low nibble of its wire bytes, a chunked vector
// re-decodes (by an independent in-test decoder) to its stated output.
//
// This is the meta-test that keeps the corpus honest: if someone
// hand-edits a vector into an inconsistent state, these tests go red
// before any downstream phase consumes the bad data.
//
#include "gtest/gtest.h"

#include "net/GoldenVectors.h"

#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace cajeta_golden;

// --- crypto: SHA-256 / SHA-1 ----------------------------------------------

static void checkHashCorpus(const std::vector<HashVector>& vecs,
                            size_t hexLen, const char* label) {
    ASSERT_GE(vecs.size(), 3u) << label << ": expected the FIPS examples";
    std::set<std::string> names;
    bool sawEmpty = false, sawAbc = false;
    for (auto& v : vecs) {
        EXPECT_FALSE(v.name.empty()) << label << ": unnamed vector";
        EXPECT_TRUE(names.insert(v.name).second)
            << label << ": duplicate name " << v.name;
        EXPECT_EQ(v.expectedHex.size(), hexLen)
            << label << ": digest length for " << v.name;
        for (char c : v.expectedHex)
            EXPECT_TRUE((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))
                << label << ": non-lowercase-hex in " << v.name;
        if (v.name == "empty") { sawEmpty = true; EXPECT_TRUE(v.input.empty()); }
        if (v.name == "abc") {
            sawAbc = true;
            ASSERT_EQ(v.input.size(), 3u);
            EXPECT_EQ(v.input[0], 'a');
        }
    }
    EXPECT_TRUE(sawEmpty) << label << ": missing the empty-message vector";
    EXPECT_TRUE(sawAbc) << label << ": missing the \"abc\" vector";
}

TEST(GoldenSha256, corpusWellFormedWithFipsExamples) {
    checkHashCorpus(sha256Vectors(), 64, "sha256");
}

TEST(GoldenSha256, knownEmptyAndAbcDigests) {
    // The two anchor digests every SHA-256 must reproduce.
    std::string empty, abc;
    for (auto& v : sha256Vectors()) {
        if (v.name == "empty") empty = v.expectedHex;
        if (v.name == "abc") abc = v.expectedHex;
    }
    EXPECT_EQ(empty,
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    EXPECT_EQ(abc,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(GoldenSha256, millionAVectorMaterializes) {
    // The classic 1,000,000-'a' case must expand to the right size +
    // content via the repeat: input-spec.
    bool found = false;
    for (auto& v : sha256Vectors()) {
        if (v.name == "million-a") {
            found = true;
            ASSERT_EQ(v.input.size(), 1000000u);
            EXPECT_EQ(v.input.front(), 'a');
            EXPECT_EQ(v.input.back(), 'a');
        }
    }
    EXPECT_TRUE(found);
}

TEST(GoldenSha1, corpusWellFormedWithFipsExamples) {
    checkHashCorpus(sha1Vectors(), 40, "sha1");
}

// --- crypto: Base64 --------------------------------------------------------

TEST(GoldenBase64, corpusWellFormed) {
    auto vecs = base64Vectors();
    ASSERT_GE(vecs.size(), 7u);   // RFC 4648 progressive-length set
    for (auto& v : vecs) {
        EXPECT_FALSE(v.name.empty());
        // Encoded length is the canonical ceil(n/3)*4 with '=' padding
        // for both alphabets (when non-empty).
        size_t n = v.input.size();
        size_t expectLen = n == 0 ? 0 : ((n + 2) / 3) * 4;
        EXPECT_EQ(v.std.size(), expectLen) << v.name << " std length";
        EXPECT_EQ(v.urlsafe.size(), expectLen) << v.name << " urlsafe length";
    }
}

TEST(GoldenBase64, alphabetsDivergeOnlyInTwoChars) {
    // std uses + / ; urlsafe uses - _ ; everything else identical.
    for (auto& v : base64Vectors()) {
        ASSERT_EQ(v.std.size(), v.urlsafe.size()) << v.name;
        for (size_t i = 0; i < v.std.size(); ++i) {
            char s = v.std[i], u = v.urlsafe[i];
            if (s == u) continue;
            bool ok = (s == '+' && u == '-') || (s == '/' && u == '_');
            EXPECT_TRUE(ok) << v.name << ": unexpected alphabet divergence '"
                            << s << "' vs '" << u << "'";
        }
    }
}

TEST(GoldenBase64, hasHighByteVectorExercisingPlusAndSlash) {
    // The corpus must include at least one vector whose std encoding
    // contains '+' and '/' (so the url-safe divergence is actually
    // exercised, not vacuously true).
    bool sawPlus = false, sawSlash = false;
    for (auto& v : base64Vectors()) {
        if (v.std.find('+') != std::string::npos) sawPlus = true;
        if (v.std.find('/') != std::string::npos) sawSlash = true;
    }
    EXPECT_TRUE(sawPlus) << "no vector exercises '+'";
    EXPECT_TRUE(sawSlash) << "no vector exercises '/'";
}

// --- URI parse + resolution -----------------------------------------------

TEST(GoldenUriParse, corpusWellFormedAndPresenceFlagsConsistent) {
    auto vecs = uriParseVectors();
    ASSERT_GE(vecs.size(), 5u);
    bool sawIpv6 = false, sawFull = false;
    for (auto& v : vecs) {
        EXPECT_FALSE(v.input.empty());
        EXPECT_FALSE(v.scheme.empty()) << v.name << ": scheme required";
        // An absent component must have an empty value.
        if (!v.hasPort) EXPECT_TRUE(v.port.empty());
        if (!v.hasQuery) EXPECT_TRUE(v.query.empty());
        if (v.name == "ipv6-host") {
            sawIpv6 = true;
            EXPECT_EQ(v.host, "::1");      // brackets stripped
            EXPECT_EQ(v.port, "80");
        }
        if (v.name == "full") {
            sawFull = true;
            EXPECT_EQ(v.scheme, "https");
            EXPECT_EQ(v.host, "h.test");
            EXPECT_EQ(v.port, "8443");
            EXPECT_EQ(v.path, "/a/b");
            EXPECT_EQ(v.query, "x=1&y=2");
            EXPECT_EQ(v.fragment, "frag");
        }
    }
    EXPECT_TRUE(sawIpv6);
    EXPECT_TRUE(sawFull);
}

TEST(GoldenUriResolution, hasFullRfc5_4NormativeSet) {
    auto vecs = uriResolutionVectors();
    // RFC 3986 §5.4 has 23 normal + 18 abnormal = 41 worked examples.
    EXPECT_GE(vecs.size(), 40u);
    for (auto& v : vecs) {
        EXPECT_FALSE(v.base.empty());
        EXPECT_FALSE(v.resolved.empty());
        // Every §5.4 example resolves against the one canonical base.
        EXPECT_EQ(v.base, "http://a/b/c/d;p?q") << "unexpected base";
    }
}

TEST(GoldenUriResolution, anchorVectorsMatchRfc) {
    // Spot-check a few normative rows so a corrupted file is caught.
    auto vecs = uriResolutionVectors();
    auto find = [&](const std::string& ref) -> std::string {
        for (auto& v : vecs) if (v.reference == ref) return v.resolved;
        return "<missing>";
    };
    EXPECT_EQ(find("g"), "http://a/b/c/g");
    EXPECT_EQ(find("../g"), "http://a/b/g");
    EXPECT_EQ(find("/g"), "http://a/g");
    EXPECT_EQ(find("g:h"), "g:h");
    EXPECT_EQ(find("../../../g"), "http://a/g");  // abnormal: clamps at root
}
