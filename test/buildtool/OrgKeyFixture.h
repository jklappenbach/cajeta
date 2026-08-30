// Shared fixture for the publisher-trust tests: real ed25519 keys, real
// signatures, real envelopes.
//
// Nothing here is a stub. The signing goes through `openssl pkeyutl -sign
// -rawin`, which is the same single-shot ed25519 operation the toolchain
// verifies against, so a test that passes here would pass against a real
// signer. A hand-rolled fake signature would only ever prove that the fake
// and the verifier agreed with each other.

#pragma once

#include "cajeta/buildtool/OrgKeyDocument.h"

#include <llvm/Support/Base64.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace cajeta::buildtool::testing {

    namespace fixture_detail {
        inline int sh(const std::string& cmd) {
            int rc = std::system((cmd + " > /dev/null 2>&1").c_str());
#ifdef _WIN32
            return rc;
#else
            return WEXITSTATUS(rc);
#endif
        }
    }

    inline std::string readWholeFile(const std::filesystem::path& p) {
        std::ifstream in(p, std::ios::binary);
        std::ostringstream buf;
        buf << in.rdbuf();
        return buf.str();
    }

    inline void writeWholeFile(const std::filesystem::path& p,
                               const std::string& text) {
        std::filesystem::create_directories(p.parent_path());
        std::ofstream out(p, std::ios::binary);
        out << text;
    }

    struct TestKeyPair {
        std::filesystem::path priv;
        std::filesystem::path pub;
    };

    inline TestKeyPair makeKeyPair(const std::filesystem::path& dir,
                                   const std::string& tag) {
        std::filesystem::create_directories(dir);
        TestKeyPair kp{dir / (tag + ".key"), dir / (tag + ".pub")};
        fixture_detail::sh("openssl genpkey -algorithm ED25519 -out "
                           + kp.priv.string());
        fixture_detail::sh("openssl pkey -in " + kp.priv.string()
                           + " -pubout -out " + kp.pub.string());
        return kp;
    }

    inline RootKey rootKeyOf(const TestKeyPair& kp, const std::string& id) {
        RootKey r;
        r.id = id;
        r.pem = readWholeFile(kp.pub);
        return r;
    }

    // Detached ed25519 over `data`, raw bytes — the form
    // `cajeta archive sign` writes.
    inline std::string signWithKey(const std::filesystem::path& scratch,
                                   const std::string& data,
                                   const std::filesystem::path& priv,
                                   const std::string& tag) {
        auto in = scratch / (tag + ".in");
        auto out = scratch / (tag + ".sig");
        writeWholeFile(in, data);
        fixture_detail::sh("openssl pkeyutl -sign -rawin -inkey "
                           + priv.string() + " -in " + in.string()
                           + " -out " + out.string());
        return readWholeFile(out);
    }

    // Wrap `payload` in the signed envelope of
    // specs/schemas/org-key-document.json.
    inline std::string envelopeAround(const std::filesystem::path& scratch,
                                      const std::string& payload,
                                      const TestKeyPair& root,
                                      const std::string& rootId,
                                      const std::string& tag) {
        std::string sig = signWithKey(scratch, payload, root.priv, tag);
        std::ostringstream env;
        env << "{\"format\":1,\"root-key-id\":\"" << rootId << "\","
            << "\"payload\":\"" << llvm::encodeBase64(payload) << "\","
            << "\"signature\":\"" << llvm::encodeBase64(sig) << "\"}";
        return env.str();
    }

    // PEM contents as a JSON string literal.
    inline std::string jsonEscapePem(const std::string& pem) {
        std::string out;
        for (char c : pem) {
            if (c == '\n') out += "\\n";
            else if (c != '\r') out += c;
        }
        return out;
    }

    struct OrgDocumentSpec {
        std::string organization = "dev.cajeta";
        std::vector<std::string> namespaces = {"dev.cajeta"};
        std::string notAfter = "2030-01-01T00:00:00Z";
        std::string keyId = "k1";
        std::string keyNotBefore = "2020-01-01T00:00:00Z";
        std::string keyNotAfter = "2030-01-01T00:00:00Z";
    };

    inline std::string orgDocumentPayload(const OrgDocumentSpec& spec,
                                          const TestKeyPair& orgKey) {
        std::ostringstream p;
        p << "{\"organization\":\"" << spec.organization << "\","
          << "\"namespaces\":[";
        for (size_t i = 0; i < spec.namespaces.size(); ++i) {
            if (i) p << ",";
            p << "\"" << spec.namespaces[i] << "\"";
        }
        p << "],\"not-after\":\"" << spec.notAfter << "\","
          << "\"keys\":[{\"id\":\"" << spec.keyId << "\","
          << "\"algorithm\":\"ed25519\","
          << "\"public-key\":\"" << jsonEscapePem(readWholeFile(orgKey.pub))
          << "\",\"not-before\":\"" << spec.keyNotBefore << "\","
          << "\"not-after\":\"" << spec.keyNotAfter << "\"}]}";
        return p.str();
    }

    inline std::string orgKeyDocument(const std::filesystem::path& scratch,
                                      const OrgDocumentSpec& spec,
                                      const TestKeyPair& orgKey,
                                      const TestKeyPair& root,
                                      const std::string& rootId,
                                      const std::string& tag = "doc") {
        return envelopeAround(scratch, orgDocumentPayload(spec, orgKey),
                              root, rootId, tag);
    }

    // A signed release-metadata envelope, optionally wrapped in the plain
    // v2 resolve body the HTTP driver's typed accessors read.
    inline std::string releasePayload(const std::string& name,
                                      const std::string& version,
                                      const std::string& sha256,
                                      const std::string& organization) {
        std::ostringstream p;
        p << "{\"name\":\"" << name << "\",\"version\":\"" << version
          << "\",\"sha256\":\"" << sha256 << "\",\"organization\":\""
          << organization << "\"}";
        return p.str();
    }

} // namespace cajeta::buildtool::testing
