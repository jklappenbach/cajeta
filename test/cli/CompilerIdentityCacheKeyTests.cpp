// The cache keys carry the running compiler binary's identity
// (primavera-web 2.2.21), so a rebuilt dev compiler misses where the
// version and git hash alone would hit.

#include <gtest/gtest.h>

#include "cajeta/util/SelfPath.h"
#include "../PortableEnv.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#if defined(__linux__)
#  include <elf.h>
#endif

namespace fs = std::filesystem;

namespace {

std::string identityCompilerBinary() {
    const char* envRoot = std::getenv("CAJETA_SOURCE_ROOT");
    std::string r;
    if (envRoot && *envRoot) r = envRoot;
    else {
#ifdef CAJETA_SOURCE_ROOT_DEFAULT
        r = CAJETA_SOURCE_ROOT_DEFAULT;
#else
        r = ".";
#endif
    }
    return r + "/build/src/cajeta";
}

std::string uniqueName(const std::string& stem) {
    static std::mt19937_64 rng(std::random_device{}());
    return stem + std::to_string(rng());
}

// The discriminator `binary` prints, or empty on any failure.
std::string keyFrom(const std::string& binary) {
    fs::path log = fs::temp_directory_path() / (uniqueName("cajeta_ident_") + ".log");
    std::string cmd = "\"" + binary + "\" --print-cache-discriminator > \""
                    + log.string() + "\" 2>&1";
    int rc = std::system(cajeta_shell(cmd).c_str());
    std::ifstream in(log);
    std::string first;
    std::getline(in, first);
    in.close();
    std::error_code ec;
    fs::remove(log, ec);
    if (rc != 0) return {};
    while (!first.empty() && (first.back() == '\r' || first.back() == '\n'))
        first.pop_back();
    if (first.size() != 64) return {};
    return first;
}

#if defined(__linux__)
// File offset and length of the NT_GNU_BUILD_ID descriptor in an ELF64 file,
// or {0, 0} when it has none. Seeks, so a multi-gigabyte binary costs nothing.
std::pair<std::size_t, std::size_t> buildIdSpan(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    auto readAt = [&](std::size_t at, void* out, std::size_t n) {
        in.seekg((std::streamoff) at);
        return (bool) in.read(static_cast<char*>(out), (std::streamsize) n);
    };
    Elf64_Ehdr eh;
    if (!readAt(0, &eh, sizeof(eh))) return {0, 0};
    if (std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 || eh.e_ident[EI_CLASS] != ELFCLASS64)
        return {0, 0};
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (!readAt(eh.e_phoff + (std::size_t) i * eh.e_phentsize, &ph, sizeof(ph))) return {0, 0};
        if (ph.p_type != PT_NOTE) continue;
        std::size_t p = ph.p_offset, end = ph.p_offset + ph.p_filesz;
        while (p + sizeof(Elf64_Nhdr) <= end) {
            Elf64_Nhdr nh;
            char name[4] = {};
            if (!readAt(p, &nh, sizeof(nh))) return {0, 0};
            std::size_t desc = p + sizeof(nh) + ((nh.n_namesz + 3) & ~3u);
            if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4
                && readAt(p + sizeof(nh), name, 4) && std::memcmp(name, "GNU", 4) == 0)
                return {desc, nh.n_descsz};
            p = desc + ((nh.n_descsz + 3) & ~3u);
        }
    }
    return {0, 0};
}

std::string bytesAt(const fs::path& path, std::size_t at, std::size_t n) {
    std::ifstream in(path, std::ios::binary);
    in.seekg((std::streamoff) at);
    std::string b(n, '\0');
    in.read(b.data(), (std::streamsize) n);
    return b;
}

std::string hexOf(const char* b, std::size_t n) {
    static const char* hex = "0123456789abcdef";
    std::string s;
    for (std::size_t i = 0; i < n; ++i) {
        s += hex[((unsigned char) b[i]) >> 4];
        s += hex[((unsigned char) b[i]) & 0xF];
    }
    return s;
}
#endif

}  // namespace

TEST(CompilerIdentityCacheKeyTests, identityNamesTheVersionAndTheBinary) {
    const std::string id = cajeta::util::compilerIdentity();
    EXPECT_NE(id.find('+'), std::string::npos) << id;
    const bool binary = id.find("+bid=") != std::string::npos
                     || id.find("+bin=") != std::string::npos;
    EXPECT_TRUE(binary) << "no binary identity in " << id;
    EXPECT_EQ(id, cajeta::util::compilerIdentity());
}

#if defined(__linux__)
TEST(CompilerIdentityCacheKeyTests, identityCarriesThisBinarysBuildId) {
    const fs::path self = cajeta::util::runningExecutablePath();
    auto [at, n] = buildIdSpan(self);
    const std::string id = cajeta::util::compilerIdentity();
    if (n == 0) {
        EXPECT_NE(id.find("+bin="), std::string::npos)
            << "no build id in the file, so the identity must fall back to size and mtime: " << id;
        return;
    }
    EXPECT_NE(id.find("+bid=" + hexOf(bytesAt(self, at, n).data(), n)), std::string::npos) << id;
}

TEST(CompilerIdentityCacheKeyTests, aRebuiltCompilerMissesAndACopyHits) {
    const fs::path original = identityCompilerBinary();
    ASSERT_TRUE(fs::exists(original)) << original;
    const std::string base = keyFrom(original.string());
    ASSERT_EQ(64u, base.size()) << "no key from " << original;

    fs::path dir = fs::temp_directory_path() / uniqueName("cajeta_ident_dir_");
    fs::create_directories(dir);
    fs::path copy = dir / "cajeta";
    fs::copy_file(original, copy);
    fs::permissions(copy, fs::perms::owner_all, fs::perm_options::add);

    auto [at, n] = buildIdSpan(copy);
    const std::string copied = keyFrom(copy.string());
    if (n == 0) {
        EXPECT_NE(base, copied) << "without a build id a fresh file must miss";
    } else {
        EXPECT_EQ(base, copied) << "a byte-identical compiler must share the cache";
        {
            std::fstream io(copy, std::ios::binary | std::ios::in | std::ios::out);
            char b = bytesAt(copy, at, 1)[0];
            b = (char) (b ^ 0x5a);
            io.seekp((std::streamoff) at);
            io.write(&b, 1);
        }
        const std::string rebuilt = keyFrom(copy.string());
        ASSERT_EQ(64u, rebuilt.size());
        EXPECT_NE(base, rebuilt) << "a compiler with another build id reused the key";
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
}
#endif
