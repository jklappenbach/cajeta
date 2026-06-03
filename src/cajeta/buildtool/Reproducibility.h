// Reproducible-build plumbing — Phase 11.
//
// The contract: same source bytes + same lockfile + same toolchain
// version → byte-identical `.cja` archive (and byte-identical
// `executable` / `archived-ir` outputs). Three inputs that would
// otherwise contribute non-determinism need to be pinned:
//
//   1. Build timestamps    → SOURCE_DATE_EPOCH (Reproducible Builds
//                            standard).
//   2. Path-dependent debug info → -fdebug-prefix-map equivalent
//                            rewrites absolute paths to repo-
//                            relative form.
//   3. RNG seeds the compiler uses internally → seeded from the
//                            source content's hash, not OS entropy.
//
// This header exposes the helpers; BuildAction / PackageAction
// thread them into the compiler invocation.

#pragma once

#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <llvm/Support/Error.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // Resolve the SOURCE_DATE_EPOCH the build will use. Precedence
    // (highest first):
    //
    //   1. CAJETA_SOURCE_DATE_EPOCH environment override (CI knob).
    //   2. SOURCE_DATE_EPOCH environment variable
    //      (the reproducible-builds.org standard).
    //   3. Manifest property `cajeta.source-date-epoch` (string,
    //      Unix timestamp).
    //   4. Default: "0" — explicitly pinned, so any timestamp the
    //      compiler emits is deterministic.
    //
    // Returns the chosen value as a Unix-timestamp string (the form
    // the compiler / linker / tar all expect).
    std::string resolveSourceDateEpoch(
        const ResolvedProperties& props,
        const std::map<std::string, std::string>& envOverrides = {});

    // Compose the debug-prefix-map argv entry the compiler should
    // pass through to LLVM. Maps the absolute project root to a
    // stable virtual prefix so the resulting debug info doesn't
    // contain the host's directory layout.
    //
    // Default virtual prefix is `cajeta:` — short, unambiguous, and
    // not a legal filesystem path on any host we care about.
    // Returns the empty string when `projectRoot` is empty (no
    // remap to apply).
    std::string composeDebugPrefixMap(
        const std::string& projectRoot,
        const std::string& virtualPrefix = "cajeta:");

    // Deterministic 64-bit seed derived from a content hash. The
    // compiler / link step / IR-cache hashing all consult this so
    // any internal-RNG-driven decision (symbol-uniquing suffixes,
    // hash-map ordering bias) is reproducible across machines.
    //
    // Implementation: SHA-256(contentHash || "cajeta/seed-v1")
    // truncated to 64 bits (little-endian).
    uint64_t deterministicSeed(const std::string& contentHash);

    // Compose the argv flags BuildAction should pass through to
    // the compiler to lock down determinism. Returns the new flags
    // in declaration order — the caller appends them to the
    // existing argv before fork+exec.
    //
    // Concretely, the v1 set is:
    //
    //   --source-date-epoch=<resolved-epoch>
    //   --debug-prefix-map=<projectRoot>=cajeta:
    //   --seed=<deterministic-seed-hex>     (when contentHash given)
    //
    // Callers that don't have a content hash yet can pass an empty
    // string to skip the `--seed` entry.
    std::vector<std::string> reproducibilityFlags(
        const ResolvedProperties& props,
        const std::string& projectRoot,
        const std::string& contentHash = "");

    // Byte-compare two files. Returns the empty string when they're
    // identical; otherwise returns a one-line diagnostic of the
    // form "size A=…, B=…; first-differing byte at offset N: A=…,
    // B=…". Designed for the rebuild-and-compare CI verifier:
    // failure cases need actionable output, not just a boolean.
    std::string byteCompareFiles(const std::string& a,
                                 const std::string& b);

    // Result of `verifyReproducibleArchive` for the CI verifier.
    struct ReproducibilityVerifyResult {
        bool identical = false;
        // Empty when identical; one-line diagnostic otherwise.
        std::string diff;
        // Sizes recorded for log lines.
        size_t sizeA = 0;
        size_t sizeB = 0;
    };

    // Compare two archives produced by independent builds of the
    // same source + lockfile. Implementation today is a pure byte
    // compare; future phases may compare on archive logical
    // content (so a sub-component mtime drift surfaces as "the
    // manifest inside the archive moved at byte K" rather than
    // "byte K differs").
    ReproducibilityVerifyResult verifyReproducibleArchive(
        const std::string& archiveA,
        const std::string& archiveB);

} // namespace cajeta::buildtool
