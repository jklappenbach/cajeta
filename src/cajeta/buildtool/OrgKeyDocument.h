// The organization key document — publisher-trust spec §2.
//
// The signed statement binding an organization to its signing keys and the
// namespaces it owns. A signature that verifies against a key in here means
// "the organization that owns this name published these bytes"; a signature
// verified against a key merely sitting in the local trust store means only
// that somebody signed them. That difference is the whole point of this
// type, and it is the one PyPI's GPG support never had.
//
// Wire format: specs/schemas/org-key-document.json. The payload travels as
// opaque bytes and the signature covers them exactly as transmitted, so
// there is no canonical-JSON step for a signer and a verifier to disagree
// about.

#pragma once

#include <llvm/Support/Error.h>

#include <ctime>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // A trust anchor: a root public key this client accepts. Carries PEM
    // CONTENTS rather than a path because the shipped root lives in the
    // binary and has no path — and one representation beats two.
    struct RootKey {
        std::string id;
        std::string pem;
        bool shipped = false;   // came with the toolchain (spec §3.1)
    };

    struct OrgSigningKey {
        std::string id;
        std::string algorithm;      // "ed25519"
        std::string publicKeyPem;   // SubjectPublicKeyInfo
        std::time_t notBefore = 0;
        std::time_t notAfter = 0;

        bool usableAt(std::time_t now) const {
            return now >= notBefore && now < notAfter;
        }
    };

    struct OrgKeyDocument {
        std::string organization;
        std::vector<std::string> namespaces;
        std::vector<OrgSigningKey> keys;
        std::time_t notAfter = 0;   // the document's own expiry
        std::string rootKeyId;      // which root signed it (spec §6.3)

        // Keys inside their validity window at `now`. Empty is a legitimate
        // answer for a document whose keys have all expired, and the caller
        // must treat it as "cannot verify", never as "verified".
        std::vector<const OrgSigningKey*> usableKeys(std::time_t now) const;
    };

    // Parse and verify an envelope.
    //
    // `now` is a PARAMETER rather than read from the clock so expiry is
    // testable without sleeping, and so a caller can pin the instant a whole
    // resolve is evaluated against instead of racing midnight partway
    // through.
    //
    // Fails when: the envelope or payload is malformed, the format version
    // is unrecognised, the signature verifies against none of `rootPemPaths`,
    // or the document has expired. An expired document is an ERROR and not a
    // parsed-but-unusable value: nothing downstream should be able to hold
    // one and forget to check (spec §2.5).
    llvm::Expected<OrgKeyDocument> loadOrgKeyDocument(
        const std::string& envelopeJson,
        const std::vector<RootKey>& roots,
        std::time_t now);

    // RFC 3339, UTC, seconds precision, `Z` only — the schema's `timestamp`.
    // Offsets are rejected rather than converted.
    llvm::Expected<std::time_t> parseUtcTimestamp(const std::string& text);

} // namespace cajeta::buildtool
