// Reproducible-build plumbing: same source, lockfile and toolchain must give a
// byte-identical archive, which means pinning build timestamps, path-dependent
// debug info, and the compiler's internal RNG seeds. BuildAction threads these.

#pragma once

#include "cajeta/buildtool/Properties.h"


#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    /// The build's SOURCE_DATE_EPOCH as a Unix-timestamp string, taken in
    /// order from CAJETA_SOURCE_DATE_EPOCH, SOURCE_DATE_EPOCH, the manifest
    /// property `cajeta.source-date-epoch`, then "0" as an explicit pin.
    std::string resolveSourceDateEpoch(
        const ResolvedProperties& props,
        const std::map<std::string, std::string>& envOverrides = {});

    /// The debug-prefix-map argv entry mapping the absolute project root to a
    /// virtual prefix, so debug info carries no host directory layout. Empty
    /// when `projectRoot` is, since there is then nothing to remap.
    std::string composeDebugPrefixMap(
        const std::string& projectRoot,
        const std::string& virtualPrefix = "cajeta:");

    /// SHA-256(contentHash || "cajeta/seed-v1") truncated to 64 bits, little-
    /// endian. Every internal-RNG decision consults it, so symbol-uniquing and
    /// hash ordering come out the same on every machine.
    uint64_t deterministicSeed(const std::string& contentHash);

    /// The determinism flags to append to the compiler argv, in declaration
    /// order: --source-date-epoch, --debug-prefix-map, and --seed. An empty
    /// `contentHash` skips the --seed entry.
    std::vector<std::string> reproducibilityFlags(
        const ResolvedProperties& props,
        const std::string& projectRoot,
        const std::string& contentHash = "");

    /// Byte-compares two files, returning "" when identical and otherwise a
    /// one-line diagnostic naming the sizes and the first differing offset.
    std::string byteCompareFiles(const std::string& a,
                                 const std::string& b);

    /// Result of `verifyReproducibleArchive`: `diff` is empty when identical.
    struct ReproducibilityVerifyResult {
        bool identical = false;
        std::string diff;
        size_t sizeA = 0;
        size_t sizeB = 0;
    };

    /// Compares two archives built independently from the same source and
    /// lockfile. A pure byte compare, not a logical-content one.
    ReproducibilityVerifyResult verifyReproducibleArchive(
        const std::string& archiveA,
        const std::string& archiveB);

} // namespace cajeta::buildtool
