// SLSA-style build attestation: the `publish` action generates an in-toto
// Statement v1 envelope whose predicate is a SLSA v1 provenance record, signed
// with the archive's key and shipped alongside it as `<archive>.attestation`.

#pragma once

#include <llvm/Support/Error.h>

#include <string>

namespace cajeta::buildtool {

    // Inputs for composing a provenance record; all strings are already rendered.
    struct ProvenanceInputs {
        // Subject:
        std::string archiveName;       // e.g. "dev.cajeta.http-1.2.4.cja"
        std::string archiveSha256;      // "sha256:<hex>"
        // Build definition:
        std::string manifestChecksum;
        std::string lockfileChecksum;
        std::string compilerVersion;
        std::string flavor;             // "release" / "debug" / …
        std::string target;             // "x86_64-linux-gnu" / "host"
        // Run details:
        std::string builderId;          // "https://github.com/cajeta-org/builder"
        std::string startedOn;          // ISO 8601
        std::string finishedOn;         // ISO 8601
    };

    // Compose the JSON Statement envelope, canonicalized (sorted keys, 2-space
    // indent) so its SHA-256 is stable across runs - a precondition for signing.
    std::string composeProvenanceJson(const ProvenanceInputs& in);

    // Verify a provenance document's structure: statement and predicate types, that
    // subject[].digest.sha256 equals `expectedSha256`, and that the checksum and
    // compiler-version fields are populated. Returns its buildDefinition fields.
    struct ProvenanceVerifyResult {
        std::string buildType;
        std::string compilerVersion;
        std::string flavor;
        std::string target;
        std::string manifestChecksum;
        std::string lockfileChecksum;
    };
    llvm::Expected<ProvenanceVerifyResult> verifyProvenanceJson(
        const std::string& jsonDoc,
        const std::string& expectedSha256);

} // namespace cajeta::buildtool
