// Release metadata — publisher-trust spec §5.1, §6.2.
//
// What the repository says about one release: its hash, and the
// organization that owns the name. Both travel inside the root-signed
// envelope, so a mirror can serve the bytes but cannot restate what they
// are or who published them.
//
// `signedByRoot` is the field every caller has to read. An unsigned
// document parses fine and is useful for diagnostics, but it authorises
// nothing: an attacker writes an unsigned `organization` as freely as a
// name prefix, which is exactly the substitution spec §4.4 exists to
// forbid.

#pragma once

#include "cajeta/buildtool/SignedEnvelope.h"

#include <llvm/Support/Error.h>

#include <string>
#include <vector>

namespace cajeta::buildtool {

    struct ReleaseMetadata {
        std::string name;
        std::string version;
        std::string sha256;         // "sha256:<hex>", normalised
        std::string organization;   // who owns the name (spec 6.2)

        // True when the fields above came out of a VERIFIED envelope —
        // signed by a root, or by a key the root delegated to. False means
        // they were read from a plain document and must not be used to
        // decide a publisher binding.
        bool signedByRoot = false;
        std::string rootKeyId;      // which root signed it (spec 6.3)

        // Withdrawn by its publisher (spec 7.6.2). Read from the SIGNED
        // payload when there is one: the plain `retracted` beside it is
        // advisory, and a mirror clears it freely.
        bool retracted = false;
        std::string retractedReason;
    };

    // Parse release metadata, verifying it when it is signed.
    //
    // Accepts three shapes, so one reader serves the HTTP and filesystem
    // drivers and the servers that predate signing:
    //   * a bare signed envelope,
    //   * an object carrying the envelope under `signed` alongside a plain
    //     v2 resolve body — the shape that keeps `/v2/resolve` readable by
    //     clients that do not verify,
    //   * a plain object, which yields `signedByRoot = false`.
    //
    // When a `signed` envelope is present it is authoritative: the plain
    // fields beside it are never merged in. A mirror that rewrites the
    // unsigned half changes nothing a verifying client reads.
    // `verifiers` are the public keys permitted to have signed this. That is
    // the delegated release keys when the repository serves a delegation
    // (spec 2.7), and the roots themselves when it does not — a repository
    // whose root signs release metadata directly still works, which is what
    // makes the delegation adoptable without a flag day. A root signature is
    // strictly stronger evidence than a delegated one, so accepting both
    // costs the client nothing; keeping the root offline is olla's
    // operational discipline, not something a client can police.
    llvm::Expected<ReleaseMetadata> loadReleaseMetadata(
        const std::string& json,
        const std::vector<RootKey>& verifiers);

} // namespace cajeta::buildtool
