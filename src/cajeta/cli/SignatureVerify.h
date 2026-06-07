// Verify a detached ed25519 signature over an archive against the
// trust store. Phase 10.
//
// Workflow:
//   1. Determine which key signed: read `<archive>.sig.keyid` (a
//      sidecar written by sign action / `cajeta archive sign`),
//      OR honor the caller's explicit `--key-id` override.
//   2. Look the key up in the trust store (env → user → system).
//   3. EVP_DigestVerify the detached signature against the archive
//      bytes.
//   4. On tampered archive: error includes the computed digest
//      (sha256 of the archive bytes) so operators can confirm
//      vs. the expected value out-of-band.

#pragma once

#include "cajeta/cli/TrustStore.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>

namespace cajeta::cli {

    struct VerifyOptions {
        // Override the key-id to look up (otherwise read from
        // `<archive>.sig.keyid`).
        std::optional<std::string> keyIdOverride;
        // Override the signature path (otherwise `<archive>.sig`).
        std::optional<std::string> signaturePathOverride;
    };

    struct VerifyResult {
        std::string keyId;          // the key the verify ran against
        std::string fingerprint;    // its fingerprint, for surfacing
        std::string archiveSha256;  // sha256 of the verified bytes
    };

    // Verify an archive's signature. Errors when:
    //   - No `<archive>.sig` / signaturePathOverride.
    //   - No `<archive>.sig.keyid` (and no keyIdOverride).
    //   - key-id not found in any trust-store tier.
    //   - signature doesn't verify against the archive bytes —
    //     error names computed vs expected digest pair.
    llvm::Expected<VerifyResult> verifyArchiveSignature(
        const TrustStoreLayout& layout,
        const std::string& archivePath,
        const VerifyOptions& opts = {});

    // Convenience: read the key-id sidecar file `<archive>.sig.keyid`.
    // Returns the stripped first line. Empty when the file is absent
    // or empty.
    std::string readKeyIdSidecar(const std::string& archivePath);

    // Write the key-id sidecar. Used by the sign path so verify can
    // resolve the matching public key without out-of-band metadata.
    llvm::Error writeKeyIdSidecar(const std::string& archivePath,
                                  const std::string& keyId);

} // namespace cajeta::cli
