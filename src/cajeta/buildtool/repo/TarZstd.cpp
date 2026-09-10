#include "cajeta/buildtool/repo/TarZstd.h"

// ZSTD_getErrorCode sits behind ZSTD_STATIC_LINKING_ONLY in newer zstd headers, so
// this define must precede the include; the symbol is exported by shared libzstd.
#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>
#include <zstd_errors.h>

#include <array>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    namespace {

        llvm::Error err(const std::string& msg) {
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(), msg);
        }

        constexpr size_t kBlock = 512;
        constexpr size_t kMaxName = 100;

        void padToBlock(std::string& out) {
            size_t rem = out.size() % kBlock;
            if (rem != 0) {
                out.append(kBlock - rem, '\0');
            }
        }

        // Write `value` as octal into a fixed-width field; the last byte stays NUL.
        void writeOctal(char* dst, size_t width, uint64_t value) {
            std::array<char, 32> buf{};
            std::snprintf(buf.data(), buf.size(), "%0*llo",
                          static_cast<int>(width - 1),
                          static_cast<unsigned long long>(value));
            std::memcpy(dst, buf.data(), width - 1);
            dst[width - 1] = '\0';
        }

        // The ustar checksum: every header byte summed, chksum read as eight spaces.
        uint32_t headerChecksum(const std::array<char, kBlock>& hdr) {
            uint32_t sum = 0;
            for (size_t i = 0; i < kBlock; ++i) {
                if (i >= 148 && i < 156) {
                    sum += static_cast<unsigned char>(' ');
                } else {
                    sum += static_cast<unsigned char>(hdr[i]);
                }
            }
            return sum;
        }

        // Append one POSIX ustar header for a regular file.
        llvm::Error appendHeader(std::string& out,
                                 const std::string& name,
                                 uint64_t size) {
            if (name.empty() || name.size() >= kMaxName) {
                return err("tar: name out of range (1.." +
                           std::to_string(kMaxName - 1) +
                           " bytes): '" + name + "'");
            }
            std::array<char, kBlock> hdr{};
            std::memcpy(hdr.data(), name.data(), name.size());
            // ustar offsets: mode 100, uid 108, gid 116, size 124, mtime 136, chksum
            // 148, typeflag 156, magic 257. mtime 0 keeps a bundle byte-identical.
            writeOctal(hdr.data() + 100, 8, 0644);
            writeOctal(hdr.data() + 108, 8, 0);
            writeOctal(hdr.data() + 116, 8, 0);
            writeOctal(hdr.data() + 124, 12, size);
            writeOctal(hdr.data() + 136, 12, 0);
            hdr[156] = '0';
            std::memcpy(hdr.data() + 257, "ustar", 5);
            hdr[262] = '\0';
            hdr[263] = '0';
            hdr[264] = '0';
            std::memset(hdr.data() + 148, ' ', 8);
            uint32_t cksum = headerChecksum(hdr);
            writeOctal(hdr.data() + 148, 7, cksum);
            hdr[155] = ' ';
            out.append(hdr.data(), kBlock);
            return llvm::Error::success();
        }

        // Parse a NUL- or space-terminated octal field; 0 when empty.
        uint64_t parseOctal(const char* p, size_t width) {
            uint64_t v = 0;
            for (size_t i = 0; i < width; ++i) {
                char c = p[i];
                if (c == '\0' || c == ' ') break;
                if (c >= '0' && c <= '7') {
                    v = (v << 3) + (c - '0');
                }
            }
            return v;
        }

    } // namespace

    llvm::Expected<std::string> writeTarZstd(
        const std::vector<TarEntry>& entries) {
        std::string tar;
        tar.reserve(entries.size() * (kBlock + 1024));
        for (const auto& e : entries) {
            if (auto err = appendHeader(tar, e.name, e.data.size())) {
                return std::move(err);
            }
            tar.append(e.data);
            padToBlock(tar);
        }
        tar.append(2 * kBlock, '\0');

        size_t bound = ::ZSTD_compressBound(tar.size());
        std::string out;
        out.resize(bound);
        size_t written = ::ZSTD_compress(
            out.data(), bound,
            tar.data(), tar.size(),
            /*level*/ 3);
        if (::ZSTD_isError(written)) {
            return err(std::string("zstd: compress failed: ") +
                       ::ZSTD_getErrorName(written));
        }
        out.resize(written);
        return out;
    }

    llvm::Expected<std::vector<TarEntry>> readTarZstd(
        const std::string& zstdBytes) {
        unsigned long long origSize =
            ::ZSTD_getFrameContentSize(zstdBytes.data(),
                                       zstdBytes.size());
        if (origSize == ZSTD_CONTENTSIZE_ERROR) {
            return err("zstd: input is not a valid zstd frame");
        }
        std::string tar;
        if (origSize == ZSTD_CONTENTSIZE_UNKNOWN) {
            // No frame content size: grow the buffer until decompress succeeds.
            tar.resize(zstdBytes.size() * 4 + 4096);
            for (;;) {
                size_t written = ::ZSTD_decompress(
                    tar.data(), tar.size(),
                    zstdBytes.data(), zstdBytes.size());
                if (!::ZSTD_isError(written)) {
                    tar.resize(written);
                    break;
                }
                if (::ZSTD_getErrorCode(written) ==
                    ZSTD_error_dstSize_tooSmall) {
                    tar.resize(tar.size() * 2);
                    continue;
                }
                return err(std::string("zstd: decompress failed: ") +
                           ::ZSTD_getErrorName(written));
            }
        } else {
            tar.resize(origSize);
            size_t written = ::ZSTD_decompress(
                tar.data(), tar.size(),
                zstdBytes.data(), zstdBytes.size());
            if (::ZSTD_isError(written)) {
                return err(std::string("zstd: decompress failed: ") +
                           ::ZSTD_getErrorName(written));
            }
            tar.resize(written);
        }

        std::vector<TarEntry> entries;
        size_t pos = 0;
        while (pos + kBlock <= tar.size()) {
            // One zero block ends the read; real archives pad past the canonical two.
            bool allZero = true;
            for (size_t i = 0; i < kBlock; ++i) {
                if (tar[pos + i] != '\0') { allZero = false; break; }
            }
            if (allZero) break;

            const char* hdr = tar.data() + pos;
            std::string name(hdr, ::strnlen(hdr, kMaxName));
            uint64_t sz = parseOctal(hdr + 124, 12);
            pos += kBlock;
            if (pos + sz > tar.size()) {
                return err("tar: entry '" + name +
                           "' size " + std::to_string(sz) +
                           " runs past end of archive");
            }
            TarEntry e;
            e.name = std::move(name);
            e.data.assign(tar.data() + pos, sz);
            entries.push_back(std::move(e));
            pos += sz;
            size_t rem = sz % kBlock;
            if (rem != 0) pos += kBlock - rem;
        }
        return entries;
    }

} // namespace cajeta::buildtool
