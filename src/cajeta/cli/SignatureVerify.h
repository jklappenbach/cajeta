// Verify a detached ed25519 signature over an archive against the trust store:
// the signing key comes from the `<archive>.sig.keyid` sidecar (or an explicit
// override), is looked up env → user → system, then EVP_DigestVerify runs.

#pragma once

#include "cajeta/cli/TrustStore.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>

namespace cajeta::cli {

    struct VerifyOptions {
        std::optional<std::string> keyIdOverride;          // else the .sig.keyid sidecar
        std::optional<std::string> signaturePathOverride;  // else `<archive>.sig`
    };

    struct VerifyResult {
        std::string keyId;          // the key the verify ran against
        std::string fingerprint;    // its fingerprint, for surfacing
        std::string archiveSha256;  // sha256 of the verified bytes
    };

    // Verify an archive's signature. Errors when the signature or key-id is
    // missing, when the key-id is in no trust-store tier, or when the signature
    // does not verify — that last error names the computed and expected digests.
    llvm::Expected<VerifyResult> verifyArchiveSignature(
        const TrustStoreLayout& layout,
        const std::string& archivePath,
        const VerifyOptions& opts = {});

    // The stripped first line of `<archive>.sig.keyid`; empty when absent.
    std::string readKeyIdSidecar(const std::string& archivePath);

    // Write that sidecar, so verify resolves the public key with no other input.
    llvm::Error writeKeyIdSidecar(const std::string& archivePath,
                                  const std::string& keyId);

} // namespace cajeta::cli
