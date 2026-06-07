//
// GoldenVectorsTests.cpp — self-validation of the NET-13.2 corpus.
//
// NET-13.2 ships *data* (checked-in golden vectors) that later phases
// (NET-6/7/10/11) pin their parsers against. The parsers don't exist
// yet, so this suite validates the corpus *itself*: every file loads,
// the field shapes are right, and each vector is internally consistent
// at a level that needs no cajeta.net parser — e.g. a SHA-256 digest
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

// --- HTTP messages ---------------------------------------------------------

TEST(GoldenHttp, manifestFilesLoadByteExactWithCrlf) {
    auto vecs = httpMessageVectors();
    ASSERT_GE(vecs.size(), 5u);
    for (auto& m : vecs) {
        EXPECT_FALSE(m.bytes.empty()) << m.file << ": empty wire bytes";
        EXPECT_TRUE(m.kind == "request" || m.kind == "response") << m.file;
        // Wire messages must use CRLF (a header is present, so a CR
        // must appear) and the head must end with the blank-line CRLFCRLF.
        std::string s(m.bytes.begin(), m.bytes.end());
        EXPECT_NE(s.find("\r\n"), std::string::npos)
            << m.file << ": missing CRLF (line endings got normalized?)";
        EXPECT_NE(s.find("\r\n\r\n"), std::string::npos)
            << m.file << ": missing header terminator";
    }
}

TEST(GoldenHttp, contentLengthFramingMatchesActualBodyBytes) {
    for (auto& m : httpMessageVectors()) {
        if (m.framing != "content-length") continue;
        std::string s(m.bytes.begin(), m.bytes.end());
        size_t sep = s.find("\r\n\r\n");
        ASSERT_NE(sep, std::string::npos) << m.file;
        size_t bodyLen = s.size() - (sep + 4);
        EXPECT_EQ((long)bodyLen, m.bodyLen)
            << m.file << ": manifest body-len disagrees with the wire body";
    }
}

TEST(GoldenHttp, startLineAppearsInWireBytes) {
    for (auto& m : httpMessageVectors()) {
        std::string s(m.bytes.begin(), m.bytes.end());
        // The manifest start-line tokens must appear at the very start.
        // request: "GET /hello.txt" ; response: "200 OK" appears after
        // the "HTTP/1.1 " version token.
        if (m.kind == "request") {
            EXPECT_EQ(s.rfind(m.startLine, 0), 0u)
                << m.file << ": request line mismatch";
        } else {
            EXPECT_NE(s.find(m.startLine), std::string::npos)
                << m.file << ": status/reason not found";
        }
    }
}

TEST(GoldenHttpChunked, reDecodeMatchesExpected) {
    auto vecs = chunkedVectors();
    ASSERT_GE(vecs.size(), 3u);
    for (auto& v : vecs) {
        // Independent in-test chunked decoder (NOT the product parser —
        // that's NET-7.4) so the vector's stated decode is self-checked.
        const auto& c = v.chunked;
        std::vector<uint8_t> out;
        size_t i = 0;
        bool ok = true;
        while (i < c.size()) {
            // read hex size line up to CRLF (ignore ;extensions)
            std::string sizeLine;
            while (i + 1 < c.size() && !(c[i] == '\r' && c[i + 1] == '\n')) {
                sizeLine.push_back((char)c[i]); ++i;
            }
            i += 2;  // skip CRLF
            size_t semi = sizeLine.find(';');
            std::string sz = semi == std::string::npos ? sizeLine
                                                       : sizeLine.substr(0, semi);
            long n = std::strtol(sz.c_str(), nullptr, 16);
            if (n == 0) break;  // last chunk
            for (long k = 0; k < n && i < c.size(); ++k) out.push_back(c[i++]);
            i += 2;  // skip trailing CRLF after chunk data
        }
        EXPECT_EQ(out, v.decoded)
            << v.name << ": chunked re-decode disagrees with stated output";
        (void)ok;
    }
}

TEST(GoldenHttpAbuse, generatorsMaterializeExpectedShapes) {
    auto vecs = httpAbuseVectors();
    ASSERT_GE(vecs.size(), 4u);
    std::set<std::string> failures;
    for (auto& v : vecs) {
        auto bytes = generateAbuseCase(v.generator);
        EXPECT_FALSE(bytes.empty()) << v.name;
        failures.insert(v.expectedFailure);
        if (v.generator.rfind("header-flood:", 0) == 0) {
            // Must actually produce a large message (flood, not a stub).
            EXPECT_GT(bytes.size(), 10000u) << v.name << ": flood too small";
        }
        if (v.generator == "missing-crlf-terminator") {
            std::string s(bytes.begin(), bytes.end());
            EXPECT_EQ(s.find("\r\n\r\n"), std::string::npos)
                << v.name << ": should NOT contain the header terminator";
        }
    }
    // The taxonomy must reference real NET-7.7 subtypes.
    std::set<std::string> known = {
        "HeadersTooLarge", "MalformedMessage",
        "InvalidChunkEncoding", "UnexpectedEof"};
    for (auto& f : failures)
        EXPECT_TRUE(known.count(f)) << "unknown expected-failure: " << f;
}

// --- WebSocket -------------------------------------------------------------

TEST(GoldenWsAccept, hasRfcExampleAndKeyIsBase64) {
    auto vecs = wsAcceptVectors();
    ASSERT_GE(vecs.size(), 1u);
    bool sawRfc = false;
    for (auto& v : vecs) {
        EXPECT_FALSE(v.key.empty());
        EXPECT_FALSE(v.expectedAccept.empty());
        // The accept value is base64(SHA-1(...)) = 20 bytes -> 28 chars
        // with a single '=' pad.
        EXPECT_EQ(v.expectedAccept.size(), 28u) << v.name;
        EXPECT_EQ(v.expectedAccept.back(), '=') << v.name;
        if (v.name == "rfc-example") {
            sawRfc = true;
            EXPECT_EQ(v.key, "dGhlIHNhbXBsZSBub25jZQ==");
            EXPECT_EQ(v.expectedAccept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
        }
    }
    EXPECT_TRUE(sawRfc) << "missing the RFC 6455 §1.3 example";
    EXPECT_STREQ(wsAcceptGuid(), "258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
}

TEST(GoldenWsFrames, headerFieldsMatchWireBytes) {
    auto vecs = wsFrameVectors();
    ASSERT_GE(vecs.size(), 5u);
    bool sawHello = false, sawMasked = false, sawControl = false, saw16 = false;
    for (auto& f : vecs) {
        ASSERT_GE(f.wire.size(), 2u) << f.name << ": frame too short";
        uint8_t b0 = f.wire[0], b1 = f.wire[1];
        // FIN is the top bit of byte 0; opcode is the low nibble.
        EXPECT_EQ((b0 & 0x80) != 0, f.fin) << f.name << ": FIN mismatch";
        EXPECT_EQ(b0 & 0x0F, f.opcode) << f.name << ": opcode mismatch";
        // MASK is the top bit of byte 1.
        EXPECT_EQ((b1 & 0x80) != 0, f.masked) << f.name << ": MASK mismatch";

        // Decode the payload length field and verify it equals the
        // stated unmasked payload size.
        uint8_t len7 = b1 & 0x7F;
        size_t headerLen = 2;
        size_t payloadLen;
        if (len7 < 126) {
            payloadLen = len7;
        } else if (len7 == 126) {
            ASSERT_GE(f.wire.size(), 4u) << f.name;
            payloadLen = ((size_t)f.wire[2] << 8) | f.wire[3];
            headerLen = 4;
            saw16 = true;
        } else {
            ASSERT_GE(f.wire.size(), 10u) << f.name;
            payloadLen = 0;
            for (int k = 0; k < 8; ++k)
                payloadLen = (payloadLen << 8) | f.wire[2 + k];
            headerLen = 10;
        }
        if (f.masked) headerLen += 4;   // 4-byte masking key
        EXPECT_EQ(payloadLen, f.payload.size())
            << f.name << ": declared length != payload field";
        EXPECT_EQ(f.wire.size(), headerLen + payloadLen)
            << f.name << ": wire length != header + payload";

        // For masked frames, applying the masking key to the wire
        // payload must yield the stated (unmasked) payload.
        if (f.masked) {
            const uint8_t* key = &f.wire[headerLen - 4];
            const uint8_t* wirePayload = &f.wire[headerLen];
            for (size_t k = 0; k < payloadLen; ++k)
                EXPECT_EQ((uint8_t)(wirePayload[k] ^ key[k % 4]), f.payload[k])
                    << f.name << ": unmask mismatch at " << k;
        }

        if (f.name == "text-unmasked-hello") {
            sawHello = true;
            ASSERT_EQ(f.payload.size(), 5u);
            EXPECT_EQ(std::string(f.payload.begin(), f.payload.end()), "Hello");
        }
        if (f.name == "text-masked-hello") {
            sawMasked = true;
            EXPECT_EQ(std::string(f.payload.begin(), f.payload.end()), "Hello");
        }
        if (f.opcode == 0x9 || f.opcode == 0xA || f.opcode == 0x8)
            sawControl = true;
    }
    EXPECT_TRUE(sawHello);
    EXPECT_TRUE(sawMasked);
    EXPECT_TRUE(sawControl) << "no ping/pong/close control-frame vector";
    EXPECT_TRUE(saw16) << "no 16-bit extended-length frame vector";
}
