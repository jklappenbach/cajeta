#include "cajeta/util/SelfPath.h"

#include <cstdint>
#include <cstring>
#include <filesystem>

#if defined(__linux__)
#  include <elf.h>
#  include <link.h>
#elif defined(__APPLE__)
#  include <dlfcn.h>
#  include <mach-o/loader.h>
#endif

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif
#ifndef CAJETA_GIT_HASH
#define CAJETA_GIT_HASH "unknown"
#endif

namespace cajeta::util {

    namespace {

        std::string hexOf(const unsigned char* b, std::size_t n) {
            static const char* hex = "0123456789abcdef";
            std::string s;
            s.reserve(n * 2);
            for (std::size_t i = 0; i < n; ++i) {
                s += hex[b[i] >> 4];
                s += hex[b[i] & 0xF];
            }
            return s;
        }

#if defined(__linux__)
        struct BuildIdSearch {
            std::uintptr_t probe;
            std::string id;
        };

        int findBuildId(dl_phdr_info* info, std::size_t, void* data) {
            auto* s = static_cast<BuildIdSearch*>(data);
            bool mine = false;
            for (int i = 0; i < info->dlpi_phnum && !mine; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                std::uintptr_t lo = info->dlpi_addr + ph.p_vaddr;
                mine = ph.p_type == PT_LOAD && s->probe >= lo && s->probe < lo + ph.p_memsz;
            }
            if (!mine) return 0;
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE) continue;
                const auto* p = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
                const auto* end = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    ElfW(Nhdr) nh;
                    std::memcpy(&nh, p, sizeof(nh));
                    const unsigned char* name = p + sizeof(nh);
                    const unsigned char* desc = name + ((nh.n_namesz + 3) & ~3u);
                    if (desc + nh.n_descsz > end) break;
                    if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4
                        && std::memcmp(name, "GNU", 4) == 0) {
                        s->id = hexOf(desc, nh.n_descsz);
                        return 1;
                    }
                    p = desc + ((nh.n_descsz + 3) & ~3u);
                }
            }
            return 1;
        }
#endif

        std::string binaryContentId() {
#if defined(__linux__)
            BuildIdSearch s{reinterpret_cast<std::uintptr_t>(&binaryContentId), {}};
            dl_iterate_phdr(findBuildId, &s);
            return s.id;
#elif defined(__APPLE__)
            Dl_info info;
            if (!dladdr(reinterpret_cast<const void*>(&binaryContentId), &info) || !info.dli_fbase)
                return {};
            const auto* mh = static_cast<const mach_header_64*>(info.dli_fbase);
            const auto* lc = reinterpret_cast<const unsigned char*>(mh + 1);
            for (std::uint32_t i = 0; i < mh->ncmds; ++i) {
                load_command cmd;
                std::memcpy(&cmd, lc, sizeof(cmd));
                if (cmd.cmd == LC_UUID) {
                    uuid_command u;
                    std::memcpy(&u, lc, sizeof(u));
                    return hexOf(u.uuid, sizeof(u.uuid));
                }
                lc += cmd.cmdsize;
            }
            return {};
#else
            return {};
#endif
        }

    } // namespace

    const std::string& compilerIdentity() {
        static const std::string identity = [] {
            std::string s = std::string(CAJETA_VERSION) + "+" + CAJETA_GIT_HASH;
            std::string id = binaryContentId();
            if (!id.empty()) return s + "+bid=" + id;
            std::error_code ec;
            std::filesystem::path exe = runningExecutablePath();
            if (exe.empty()) return s;
            auto size = std::filesystem::file_size(exe, ec);
            if (ec) return s;
            auto mtime = std::filesystem::last_write_time(exe, ec);
            if (ec) return s;
            return s + "+bin=" + std::to_string((unsigned long long) size) + ":"
                 + std::to_string((long long) mtime.time_since_epoch().count());
        }();
        return identity;
    }

} // namespace cajeta::util
