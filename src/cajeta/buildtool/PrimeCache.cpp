#include "cajeta/buildtool/PrimeCache.h"

#include "cajeta/buildtool/IrCache.h"
#include "cajeta/buildtool/Lockfile.h"  // sha256Hex

#include <algorithm>
#include <fstream>

namespace cajeta::buildtool {

    namespace {
        std::string bareHex(std::string s) {
            const std::string prefix = "sha256:";
            if (s.rfind(prefix, 0) == 0) s = s.substr(prefix.size());
            return s;
        }
    }

    std::string primeDigestOver(
            std::vector<std::pair<std::string, std::string>> files,
            const std::string& preludeTag) {
        std::sort(files.begin(), files.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
        // Length-prefix every field so no (path, bytes, tag) concatenation
        // can collide with another split of the same byte stream.
        std::string canonical;
        auto append = [&canonical](const std::string& s) {
            canonical += std::to_string(s.size());
            canonical += ':';
            canonical += s;
        };
        append(preludeTag);
        for (auto& [path, bytes] : files) {
            append(path);
            append(bytes);
        }
        return bareHex(sha256Hex(canonical));
    }

    std::optional<std::string> primeValidatedLookup(
            const IrCache& cache,
            const std::string& discriminator,
            const std::string& sourceDigest) {
        auto path = cache.lookup(discriminator, sourceDigest);
        if (!path) return std::nullopt;
        std::ifstream in(*path, std::ios::binary);
        if (!in) return std::nullopt;
        char magic[4] = {0, 0, 0, 0};
        in.read(magic, 4);
        if (in.gcount() != 4) return std::nullopt;   // truncated → miss
        // LLVM bitcode magic: 'B' 'C' 0xC0 0xDE (a raw-bitcode header; the
        // wrapper format is not produced by our writer, so reject it too).
        if (magic[0] != 'B' || magic[1] != 'C'
                || (unsigned char) magic[2] != 0xC0
                || (unsigned char) magic[3] != 0xDE) {
            return std::nullopt;                      // corrupt → miss
        }
        return path;
    }

} // namespace cajeta::buildtool
