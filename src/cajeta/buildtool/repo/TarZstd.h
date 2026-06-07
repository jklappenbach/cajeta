// Phase 6d — minimal tar.zst reader + writer used by the v2 bundle
// + lockfile-diff endpoints. POSIX tar (ustar) is small enough to
// hand-roll (one 512-byte header per file, file bytes padded to
// 512, two zero-blocks end-of-archive); libzstd handles the wrap.
//
// We don't link libarchive: the buildtool's only archive use is
// these v2 bundles, which carry one .cja per entry plus a single
// `bundle.json` index — the full libarchive surface (multiple
// formats, sparse files, extended attributes, etc.) buys nothing
// here.

#pragma once

#include <llvm/Support/Error.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One entry inside a tar archive. `data` is the raw file
    // bytes (uncompressed). Filenames are flat — no directory
    // entries, no permissions, no metadata — by convention v2
    // bundles only ever ship regular files.
    struct TarEntry {
        std::string name;
        std::string data;
    };

    // Build a tar archive from `entries` and zstd-compress the
    // result. Compression level 3 (libzstd default fast/good
    // balance).
    llvm::Expected<std::string> writeTarZstd(
        const std::vector<TarEntry>& entries);

    // Decompress a zstd blob, parse the tar contents, return one
    // TarEntry per file. Empty archive → empty vector. Malformed
    // archives → Error.
    llvm::Expected<std::vector<TarEntry>> readTarZstd(
        const std::string& zstdBytes);

} // namespace cajeta::buildtool
