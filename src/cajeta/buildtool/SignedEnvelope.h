// The signed envelope — one shape for every statement the repository signs.
// The signature covers the payload bytes exactly as transmitted, so there is
// no canonical-JSON step for signer and verifier to disagree about.

#pragma once

#include <llvm/Support/Error.h>

#include <string>
#include <vector>

namespace cajeta::buildtool {

    // A trust anchor: a root public key this client accepts, held as PEM text.
    struct RootKey {
        std::string id;
        std::string pem;
        bool shipped = false;   // came with the toolchain
    };

    struct SignedEnvelope {
        std::string payload;      // decoded, byte-for-byte as it was signed
        // The root that actually VERIFIED the payload, never the one the
        // envelope claimed; its `root-key-id` is only a candidate hint.
        std::string rootKeyId;
    };

    // Parse and verify an envelope; `what` names the document in error text.
    // Fails when the envelope is malformed, the format version is unknown, or
    // the signature verifies against no root; returns nothing unverified.
    llvm::Expected<SignedEnvelope> openSignedEnvelope(
        const std::string& envelopeJson,
        const std::vector<RootKey>& roots,
        const std::string& what);

    // Whether `envelopeJson` is an envelope at all rather than a plain unsigned
    // document, so a caller can tell "this server signs" from "it does not".
    bool looksLikeSignedEnvelope(const std::string& envelopeJson);

} // namespace cajeta::buildtool
