// The organization key document - publisher-trust spec §2: the signed statement
// binding an organization to its signing keys and the namespaces it owns. The
// payload travels as opaque bytes, so the signature covers it exactly as sent.

#pragma once

#include "cajeta/buildtool/SignedEnvelope.h"

#include <llvm/Support/Error.h>

#include <ctime>
#include <string>
#include <vector>

namespace llvm { namespace json { class Array; } }

namespace cajeta::buildtool {

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

    // Where to report a vulnerability in what this organization publishes.
    struct SecurityContact {
        std::string uri;    // mailto: or https:, never a bare address
        std::string label;  // display only
    };

    struct OrgKeyDocument {
        std::string organization;
        std::vector<std::string> namespaces;
        std::vector<OrgSigningKey> keys;
        std::time_t issuedAt = 0;   // when it was produced (spec §2.9)
        std::time_t notAfter = 0;   // the document's own expiry
        std::string rootKeyId;      // which root signed it (spec §6.3)
        SecurityContact securityContact;   // empty uri when absent

        // Keys inside their validity window at `now`. Empty is legitimate, and the
        // caller must treat it as "cannot verify", never as "verified".
        std::vector<const OrgSigningKey*> usableKeys(std::time_t now) const;
    };

    // Parse a `keys` array, shared by the org key document and the repository
    // delegation so the rules have one implementation. `what` names it in errors.
    llvm::Expected<std::vector<OrgSigningKey>> parseSigningKeys(
        const llvm::json::Array& keys, const std::string& what);

    // Parse and verify an envelope against `roots`. `now` is a parameter so a whole
    // resolve pins one instant. Expiry is an ERROR, and a document older than
    // `seenIssuedAt` (0 = nothing seen yet) is REFUSED as a replay of removed keys.
    llvm::Expected<OrgKeyDocument> loadOrgKeyDocument(
        const std::string& envelopeJson,
        const std::vector<RootKey>& roots,
        std::time_t now,
        std::time_t seenIssuedAt = 0);

    // RFC 3339, UTC, seconds precision, `Z` only — the schema's `timestamp`.
    // Offsets are rejected rather than converted.
    llvm::Expected<std::time_t> parseUtcTimestamp(const std::string& text);

} // namespace cajeta::buildtool
