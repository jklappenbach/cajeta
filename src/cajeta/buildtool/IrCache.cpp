#include "cajeta/buildtool/IrCache.h"

#include "cajeta/buildtool/Lockfile.h"  // sha256Hex

#include <llvm/Support/Error.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace cajeta::buildtool {

    namespace fs = std::filesystem;

    namespace {

        llvm::Error errFs(const std::string& msg, std::error_code ec) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                msg + ": " + ec.message());
        }

        llvm::Error errMsg(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

    } // namespace

    std::string computeCacheDiscriminator(
        const std::string& compilerVersion,
        std::vector<std::pair<std::string, std::string>> flags) {
        // Sorted, so the discriminator does not depend on caller order.
        std::sort(flags.begin(), flags.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) return a.first < b.first;
                      return a.second < b.second;
                  });

        // Compiler version, then NUL-separated `k=v` pairs: NUL, unlike `=`,
        // cannot appear in a flag value and be mistaken for the separator.
        std::string canonical = compilerVersion;
        canonical.push_back('\0');
        for (const auto& [k, v] : flags) {
            canonical += k;
            canonical.push_back('=');
            canonical += v;
            canonical.push_back('\0');
        }
        std::string hex = sha256Hex(canonical);
        // Bare hex: the discriminator is a directory name, and a colon travels badly.
        const std::string prefix = "sha256:";
        if (hex.compare(0, prefix.size(), prefix) == 0) {
            hex.erase(0, prefix.size());
        }
        return hex;
    }

    IrCache::IrCache(std::string rootDir)
        : rootDir_(std::move(rootDir)) {}

    std::string IrCache::keyFor(
        const std::string& discriminator,
        const std::string& sourceDigest) const {
        fs::path p = fs::path(rootDir_) / discriminator /
                     (sourceDigest + ".bc");
        return p.string();
    }

    std::optional<std::string> IrCache::lookup(
        const std::string& discriminator,
        const std::string& sourceDigest) const {
        std::string path = keyFor(discriminator, sourceDigest);
        std::error_code ec;
        if (!fs::exists(path, ec) || ec) {
            return std::nullopt;
        }
        return path;
    }

    llvm::Error IrCache::store(
        const std::string& discriminator,
        const std::string& sourceDigest,
        const std::string& bytes) const {
        std::string target = keyFor(discriminator, sourceDigest);
        std::error_code ec;
        fs::create_directories(fs::path(target).parent_path(), ec);
        if (ec) {
            return errFs("ir-cache: cannot create '" +
                         fs::path(target).parent_path().string() + "'",
                         ec);
        }
        // Tempfile + rename in the SAME directory, so the rename is atomic.
        std::string tmp = target + ".tmp-" +
                          std::to_string(::getpid()) + "-" +
                          std::to_string(::rand());
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                return errMsg("ir-cache: cannot open '" + tmp +
                              "' for write");
            }
            out.write(bytes.data(),
                      static_cast<std::streamsize>(bytes.size()));
            if (!out) {
                std::error_code rm; fs::remove(tmp, rm);
                return errMsg("ir-cache: write failed on '" + tmp + "'");
            }
        }
        fs::rename(tmp, target, ec);
        if (ec) {
            std::error_code rm; fs::remove(tmp, rm);
            return errFs("ir-cache: rename to '" + target + "'", ec);
        }
        return llvm::Error::success();
    }

    llvm::Expected<int> IrCache::wipe() const {
        std::error_code ec;
        if (!fs::exists(rootDir_, ec)) {
            return 0;
        }
        // Counted first: fs::remove_all would lose the count.
        int removed = 0;
        for (auto it = fs::recursive_directory_iterator(rootDir_, ec);
             !ec && it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file()) ++removed;
        }
        std::error_code rmEc;
        fs::remove_all(rootDir_, rmEc);
        if (rmEc) {
            return errFs("ir-cache: cannot wipe '" + rootDir_ + "'",
                         rmEc);
        }
        return removed;
    }

    llvm::Expected<int> IrCache::evict(EvictionPolicy policy) const {
        if (policy.maxBytes == 0 && policy.maxAge.count() == 0) {
            return 0;
        }
        std::error_code ec;
        if (!fs::exists(rootDir_, ec)) {
            return 0;
        }
        struct Entry {
            std::string path;
            uint64_t size;
            std::chrono::system_clock::time_point atime;
        };
        std::vector<Entry> entries;
        for (auto it = fs::recursive_directory_iterator(rootDir_, ec);
             !ec && it != fs::recursive_directory_iterator(); ++it) {
            if (!it->is_regular_file()) continue;
            // path().c_str() is wchar_t* on Windows, but ::stat takes char*.
            std::string p = it->path().string();
            struct stat st;
            if (::stat(p.c_str(), &st) != 0) continue;
            Entry e;
            e.path = p;
            e.size = static_cast<uint64_t>(st.st_size);
            e.atime = std::chrono::system_clock::from_time_t(st.st_atime);
            entries.push_back(std::move(e));
        }
        // Oldest first: eviction walks this order until the cap is met.
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) {
                      return a.atime < b.atime;
                  });

        int removed = 0;
        auto now = std::chrono::system_clock::now();

        std::vector<Entry> survivors;
        survivors.reserve(entries.size());
        for (auto& e : entries) {
            if (policy.maxAge.count() > 0 &&
                (now - e.atime) > policy.maxAge) {
                std::error_code rm;
                fs::remove(e.path, rm);
                if (!rm) ++removed;
            } else {
                survivors.push_back(std::move(e));
            }
        }

        if (policy.maxBytes > 0) {
            uint64_t total = 0;
            for (const auto& e : survivors) total += e.size;
            for (auto& e : survivors) {
                if (total <= policy.maxBytes) break;
                std::error_code rm;
                fs::remove(e.path, rm);
                if (!rm) {
                    ++removed;
                    total -= e.size;
                }
            }
        }
        return removed;
    }

    llvm::Expected<uint64_t> IrCache::sizeBytes() const {
        std::error_code ec;
        if (!fs::exists(rootDir_, ec)) {
            return uint64_t{0};
        }
        uint64_t total = 0;
        for (auto it = fs::recursive_directory_iterator(rootDir_, ec);
             !ec && it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file()) {
                std::error_code se;
                total += fs::file_size(it->path(), se);
            }
        }
        return total;
    }

} // namespace cajeta::buildtool
