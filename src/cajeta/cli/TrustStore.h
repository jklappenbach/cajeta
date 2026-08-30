// Trust store — content-addressed registry of ed25519 public keys
// the launcher accepts as signers for signed archives. Phase 10.
//
// Layout (per BuildTool.md "Trust store"):
//   $CAJETA_TRUST_KEYS_DIR/<key-id>.pem    (env override)
//   ~/.cajeta/trust/keys/<key-id>.pem       (user store)
//   /etc/cajeta/trust/keys/<key-id>.pem     (system store; on Windows:
//                                            %ProgramData%\cajeta\trust\keys)
//
// Lookup precedence: env → user → system. The first hit wins; the
// remaining tiers are not consulted. Mutating subcommands
// (`cajeta trust add/remove`) always operate on the user store —
// the system store stays untouched so package-installed keys aren't
// disturbed by user actions. The env override is read-only.

#pragma once

#include "cajeta/buildtool/RootTrust.h"

#include <llvm/Support/Error.h>

#include <optional>
#include <string>
#include <vector>

namespace cajeta::cli {

    struct TrustStoreLayout {
        // Order matches lookup precedence: index 0 wins over 1, etc.
        // Empty path entries are skipped (e.g. when an env override
        // isn't set).
        std::vector<std::string> roots;
        // Mutating tier — always points at the user store.
        std::string userRoot;
        // For diagnostics: which path the env override pointed at
        // (empty when not set).
        std::string envRoot;
        // System path (read-only at the user-tier level).
        std::string systemRoot;
    };

    // Resolve the canonical trust-store layout. Honors:
    //   CAJETA_TRUST_KEYS_DIR  — env override (highest precedence)
    //   $HOME (or homeOverride) — user-tier root
    //   /etc/cajeta or %ProgramData%\cajeta — system-tier root
    TrustStoreLayout resolveTrustStoreLayout(
        std::optional<std::string> homeOverride = std::nullopt,
        std::optional<std::string> systemOverride = std::nullopt,
        std::optional<std::string> envOverride = std::nullopt);

    // One trust-store entry surfaced by `list`. `tier` is one of
    // "env" / "user" / "system" — what `list` prints alongside the
    // key-id to help operators understand where a key came from.
    struct TrustStoreEntry {
        std::string keyId;
        std::string path;
        std::string tier;
        std::string fingerprint;   // SHA-256 hex of the DER pubkey
    };

    // List every key visible through the precedence chain. When the
    // same key-id appears in multiple tiers, only the winning tier
    // surfaces (matches lookup behavior). Empty vector when no keys.
    std::vector<TrustStoreEntry> listTrustedKeys(
        const TrustStoreLayout& layout);

    // Look up a single key by id. Returns nullopt when no tier has it.
    std::optional<TrustStoreEntry> lookupTrustedKey(
        const TrustStoreLayout& layout,
        const std::string& keyId);

    // Add a key to the USER tier. Copies the PEM at `pemPath` into
    // <userRoot>/<keyId>.pem. Errors on:
    //   - duplicate user-tier id (use `remove` first)
    //   - source PEM not an ed25519 public key
    //   - cannot create userRoot
    llvm::Error addTrustedKey(const TrustStoreLayout& layout,
                              const std::string& keyId,
                              const std::string& pemPath);

    // Remove a key from the USER tier. Returns Error when not found
    // (so scripts can tell). Removing a key that lives in the system
    // tier is a no-op: the next list/lookup still surfaces it from
    // the system tier — the "system store unaffected" property.
    llvm::Error removeTrustedKey(const TrustStoreLayout& layout,
                                 const std::string& keyId);

    // The ROOT-key view of the same directories (publisher-trust §3.3).
    //
    // Roots and trusted keys are different things — a root vouches for an
    // organization's key document, a trusted key vouches for an artifact
    // directly — but they live in the same tiered store, and every caller
    // has to derive one from the other the same way. One conversion, so a
    // second caller cannot pick a different tier order and quietly consult
    // a different set of anchors.
    buildtool::RootTrustLayout rootTrustLayoutOf(
        const TrustStoreLayout& layout);

    // Compute the SHA-256 fingerprint of an ed25519 public key PEM.
    // Returns the hex digest of the SubjectPublicKeyInfo DER bytes —
    // matches what `ssh-keygen -lf` / `openssl pkey -pubout | sha256sum`
    // would produce.
    llvm::Expected<std::string> fingerprintOfPemFile(
        const std::string& pemPath);

} // namespace cajeta::cli
