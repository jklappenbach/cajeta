// The signed envelope — publisher-trust spec §2.3, §5.1.
//
// One shape carries every statement the repository signs: the organization
// key document (§2) and the release metadata (§5.1). The payload travels as
// opaque bytes and the signature covers them exactly as transmitted, so
// there is no canonical-JSON step for a signer and a verifier to disagree
// about. Signing a parsed-and-re-serialised object is a known
// signature-bypass class; this format makes it unrepresentable.
//
// Wire format: specs/schemas/org-key-document.json (the envelope half is
// common to both documents).

#pragma once

#include <llvm/Support/Error.h>

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

    struct SignedEnvelope {
        // The decoded payload, byte-for-byte as it was signed.
        std::string payload;
        // The id of the root that actually VERIFIED it — not the one the
        // envelope claimed. The envelope's `root-key-id` is a hint for
        // picking a candidate; reporting it as fact would let a document
        // name a root that never signed it (spec §6.3 wants the answer,
        // not the assertion).
        std::string rootKeyId;
    };

    // Parse and verify an envelope. `what` names the document in error
    // text ("organization key document", "release metadata").
    //
    // Fails when the envelope is malformed, the format version is
    // unrecognised, or the signature verifies against none of `roots`.
    // Nothing inside an unverified envelope is returned, so no caller can
    // act on a payload that has not been vouched for.
    llvm::Expected<SignedEnvelope> openSignedEnvelope(
        const std::string& envelopeJson,
        const std::vector<RootKey>& roots,
        const std::string& what);

    // Whether `envelopeJson` is an envelope at all, as opposed to a plain
    // unsigned document. Lets a caller tell "this server signs" from "this
    // server does not" without treating the second as malformed — the §5.4
    // and §9.1 legacy paths depend on that distinction.
    bool looksLikeSignedEnvelope(const std::string& envelopeJson);

} // namespace cajeta::buildtool
