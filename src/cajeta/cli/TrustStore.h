// Trust store: ed25519 public keys the launcher accepts as signers, held under
// $CAJETA_TRUST_KEYS_DIR, ~/.cajeta/trust/keys and /etc/cajeta/trust/keys.
// Lookup runs env -> user -> system, first hit wins; only the user tier is mutable.

#pragma once

#include "cajeta/buildtool/RootTrust.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::cli {

    struct TrustStoreLayout {
        // Tiers in lookup order, index 0 winning; an empty entry (unset env) is skipped.
        std::vector<std::string> roots;
        std::string userRoot;      // mutating tier; always the user store
        std::string envRoot;       // for diagnostics: where the env override pointed
        std::string systemRoot;    // read-only at the user tier
    };

    // Resolve the canonical layout from CAJETA_TRUST_KEYS_DIR, $HOME (or homeOverride)
    // and /etc/cajeta or %ProgramData%\cajeta, in that precedence.
    TrustStoreLayout resolveTrustStoreLayout(
        std::optional<std::string> homeOverride = std::nullopt,
        std::optional<std::string> systemOverride = std::nullopt,
        std::optional<std::string> envOverride = std::nullopt);

    // One entry surfaced by `list`; `tier` is "env", "user" or "system".
    struct TrustStoreEntry {
        std::string keyId;
        std::string path;
        std::string tier;
        std::string fingerprint;   // SHA-256 hex of the DER pubkey
    };

    // Every key visible through the precedence chain. A key-id present in several
    // tiers surfaces only from the winning one, matching lookup.
    std::vector<TrustStoreEntry> listTrustedKeys(
        const TrustStoreLayout& layout);

    // Look up a single key by id. Returns nullopt when no tier has it.
    std::optional<TrustStoreEntry> lookupTrustedKey(
        const TrustStoreLayout& layout,
        const std::string& keyId);

    // Copy the PEM at `pemPath` into <userRoot>/<keyId>.pem. Errors on a duplicate
    // user-tier id, a source that is not an ed25519 public key, or an uncreatable root.
    llvm::Error addTrustedKey(const TrustStoreLayout& layout,
                              const std::string& keyId,
                              const std::string& pemPath);

    // Remove a key from the USER tier, erroring when it is not there so scripts can
    // tell. A key in the system tier is untouched and still surfaces afterwards.
    llvm::Error removeTrustedKey(const TrustStoreLayout& layout,
                                 const std::string& keyId);

    // The ROOT-key view of the same tiered directories. One conversion, so no second
    // caller can pick a different tier order and consult a different set of anchors.
    buildtool::RootTrustLayout rootTrustLayoutOf(
        const TrustStoreLayout& layout);

    // SHA-256 hex of an ed25519 public key PEM's SubjectPublicKeyInfo DER bytes.
    llvm::Expected<std::string> fingerprintOfPemFile(
        const std::string& pemPath);

} // namespace cajeta::cli
