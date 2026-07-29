//
// GoldenVectors.h — loader for the NET-13.2 golden-vector corpus.
//
// The corpus (checked-in byte-exact test data under
// test/net/golden/) is the shared known-answer set every cajeta.io.net
// parser/codec phase pins against. This header parses each corpus
// file format into structured C++ records so a downstream phase can
//
//     #include "net/GoldenVectors.h"
//     for (auto& v : cajeta_golden::sha256Vectors()) { ... }
//
// and drive its implementation against the vectors. It has ZERO
// dependency on the (not-yet-built) parsers — it only reads data —
// so it lands standalone with NET-13.2 and is consumed later.
//
// Corpus root is resolved from CAJETA_TEST_ROOT (baked in by CMake to
// <project>/test, env-overridable) — the same mechanism the on-disk
// .cajeta-fixture tests use. See test/net/golden/README.md for the
// file formats + token conventions (<EMPTY>, <NONE>, input-specs).
//
#pragma once

#include "CajetaUnitTest.h"   // CAJETA_TEST_ROOT

#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cajeta_golden {

// ---------------------------------------------------------------------------
// Small parsing helpers (header-only, inline so multiple TUs can include).
// ---------------------------------------------------------------------------

inline std::string goldenRoot() {
    return CAJETA_TEST_ROOT + std::string("/net/golden");
}

inline std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Read a whole file as raw bytes (preserves CRLF in the .http corpus).
inline std::vector<uint8_t> readBytes(const std::string& relPath) {
    std::string full = goldenRoot() + "/" + relPath;
    std::ifstream in(full, std::ios::binary);
    if (!in) throw std::runtime_error("golden: cannot open " + full);
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
}

// Read a `|`-separated, `#`-comment vector file into rows of fields.
// Blank lines and lines whose first non-space char is '#' are skipped.
// Each field is trimmed of surrounding whitespace.
inline std::vector<std::vector<std::string>>
readRows(const std::string& relPath) {
    std::string full = goldenRoot() + "/" + relPath;
    std::ifstream in(full, std::ios::binary);
    if (!in) throw std::runtime_error("golden: cannot open " + full);
    std::vector<std::vector<std::string>> rows;
    std::string line;
    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        std::vector<std::string> fields;
        size_t start = 0;
        while (true) {
            size_t bar = line.find('|', start);
            if (bar == std::string::npos) {
                fields.push_back(trim(line.substr(start)));
                break;
            }
            fields.push_back(trim(line.substr(start, bar - start)));
            start = bar + 1;
        }
        rows.push_back(std::move(fields));
    }
    return rows;
}

inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    throw std::runtime_error(std::string("golden: bad hex digit '") + c + "'");
}

inline std::vector<uint8_t> hexDecode(const std::string& hex) {
    if (hex.size() % 2 != 0)
        throw std::runtime_error("golden: odd-length hex: " + hex);
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2)
        out.push_back((uint8_t)((hexNibble(hex[i]) << 4) | hexNibble(hex[i + 1])));
    return out;
}

inline std::string hexEncode(const std::vector<uint8_t>& bytes) {
    static const char* k = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) { out.push_back(k[b >> 4]); out.push_back(k[b & 0xF]); }
    return out;
}

// Decode the crypto input-spec grammar into raw message bytes.
//   empty | ascii:<text> | hex:<hexbytes> | repeat:<byte>:<n>
inline std::vector<uint8_t> decodeInputSpec(const std::string& spec) {
    if (spec == "empty") return {};
    if (spec.rfind("ascii:", 0) == 0) {
        std::string s = spec.substr(6);
        return std::vector<uint8_t>(s.begin(), s.end());
    }
    if (spec.rfind("hex:", 0) == 0) return hexDecode(spec.substr(4));
    if (spec.rfind("repeat:", 0) == 0) {
        std::string rest = spec.substr(7);
        size_t colon = rest.find(':');
        if (colon == std::string::npos || colon != 1)
            throw std::runtime_error("golden: bad repeat spec: " + spec);
        uint8_t byte = (uint8_t)rest[0];
        long n = std::stol(rest.substr(colon + 1));
        return std::vector<uint8_t>((size_t)n, byte);
    }
    throw std::runtime_error("golden: unknown input-spec: " + spec);
}

// ---------------------------------------------------------------------------
// Record types + typed loaders. (The http/ and ws/ corpus sections moved out
// with the HTTP/WebSocket stack — protocol golden vectors now live in the
// external dev.cajeta.http library's suite; crypto/ and uri/ remain.)
// ---------------------------------------------------------------------------

// crypto/sha256.vectors, crypto/sha1.vectors
struct HashVector {
    std::string name;
    std::vector<uint8_t> input;
    std::string expectedHex;   // lowercase hex digest
};

inline std::vector<HashVector> hashVectors(const std::string& relPath) {
    std::vector<HashVector> v;
    for (auto& r : readRows(relPath)) {
        if (r.size() != 3)
            throw std::runtime_error("golden: " + relPath + " expects 3 fields");
        v.push_back({r[0], decodeInputSpec(r[1]), r[2]});
    }
    return v;
}

inline std::vector<HashVector> sha256Vectors() {
    return hashVectors("crypto/sha256.vectors");
}
inline std::vector<HashVector> sha1Vectors() {
    return hashVectors("crypto/sha1.vectors");
}

// crypto/base64.vectors
struct Base64Vector {
    std::string name;
    std::vector<uint8_t> input;
    std::string std;       // standard-alphabet encoding ("" if <EMPTY>)
    std::string urlsafe;   // url-safe-alphabet encoding ("" if <EMPTY>)
};

inline std::string emptyToken(const std::string& f) {
    return f == "<EMPTY>" ? std::string() : f;
}

inline std::vector<Base64Vector> base64Vectors() {
    std::vector<Base64Vector> v;
    for (auto& r : readRows("crypto/base64.vectors")) {
        if (r.size() != 4)
            throw std::runtime_error("golden: base64.vectors expects 4 fields");
        v.push_back({r[0], decodeInputSpec(r[1]),
                     emptyToken(r[2]), emptyToken(r[3])});
    }
    return v;
}

// uri/parse.vectors
struct UriParseVector {
    std::string name, input;
    // <NONE> -> the field is std::string("\x01<NONE>") sentinel-free:
    // we expose explicit present-flags instead.
    std::string scheme, userinfo, host, port, path, query, fragment;
    bool hasUserinfo, hasHost, hasPort, hasQuery, hasFragment;
};

inline std::vector<UriParseVector> uriParseVectors() {
    std::vector<UriParseVector> v;
    for (auto& r : readRows("uri/parse.vectors")) {
        if (r.size() != 9)
            throw std::runtime_error("golden: uri/parse.vectors expects 9 fields");
        auto present = [](const std::string& f) { return f != "<NONE>"; };
        auto val = [](const std::string& f) {
            return (f == "<NONE>" || f == "<EMPTY>") ? std::string() : f;
        };
        UriParseVector u;
        u.name = r[0]; u.input = r[1];
        u.scheme = val(r[2]);
        u.userinfo = val(r[3]); u.hasUserinfo = present(r[3]);
        u.host = val(r[4]);     u.hasHost = present(r[4]);
        u.port = val(r[5]);     u.hasPort = present(r[5]);
        u.path = val(r[6]);
        u.query = val(r[7]);    u.hasQuery = present(r[7]);
        u.fragment = val(r[8]); u.hasFragment = present(r[8]);
        v.push_back(std::move(u));
    }
    return v;
}

// uri/rfc3986-resolution.vectors
struct UriResolutionVector {
    std::string base, reference, resolved;
};

inline std::vector<UriResolutionVector> uriResolutionVectors() {
    std::vector<UriResolutionVector> v;
    for (auto& r : readRows("uri/rfc3986-resolution.vectors")) {
        if (r.size() != 3)
            throw std::runtime_error("golden: uri resolution expects 3 fields");
        std::string ref = (r[1] == "<EMPTY>") ? std::string() : r[1];
        v.push_back({r[0], ref, r[2]});
    }
    return v;
}

} // namespace cajeta_golden
