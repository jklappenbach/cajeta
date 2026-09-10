// Minimal tar.zst reader + writer for v2 bundles: hand-rolled POSIX ustar
// (512-byte header per file, bytes padded to 512, two zero blocks) in libzstd.

#pragma once

#include <llvm/Support/Error.h>

#include <string>
#include <vector>

namespace cajeta::buildtool {

    // One archive entry; `data` is the raw uncompressed bytes. Names are flat —
    // v2 bundles ship regular files only, no directories, permissions or metadata.
    struct TarEntry {
        std::string name;
        std::string data;
    };

    // Tars `entries` and zstd-compresses the result at level 3.
    llvm::Expected<std::string> writeTarZstd(
        const std::vector<TarEntry>& entries);

    // Decompresses, parses the tar, and returns one entry per file. An empty
    // archive yields an empty vector; a malformed one yields an Error.
    llvm::Expected<std::vector<TarEntry>> readTarZstd(
        const std::string& zstdBytes);

} // namespace cajeta::buildtool
