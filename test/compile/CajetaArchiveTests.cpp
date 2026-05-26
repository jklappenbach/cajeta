// CajetaArchive — `.cja` container writer for cajeta library / app
// distribution. v1 implementation lands the shape spelled out in
// Compilation.md § Archive format § Minimum-viable v1:
//
//   - 32-byte header (magic "CAJETA01", format version, flags,
//     index offset/length both zero for v1).
//   - Raw-JSON manifest, length-prefixed.
//   - Length-prefixed entries each with name + origin_tag + kind_tag
//     + bytes (LLVM bitcode for class entries, raw bytes for
//     resources).
//
// These tests pin the byte-level shape so the format stays compatible
// across versions and so consumers (the `.cja` reader, the `cja` CLI
// tool, future zstd-compression work) can rely on what's there.

#include <gtest/gtest.h>
#include "cajeta/compile/CajetaArchive.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

using cajeta::CajetaArchive;
using cajeta::CajetaArchiveEntry;

namespace {

// Read a freshly-written archive's header bytes back into a vector for
// inspection. Tests use this to verify byte-level layout without going
// through CajetaArchive's own reader code (which lands later — the
// writer is the only thing here).
std::vector<uint8_t> readBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(f)),
        std::istreambuf_iterator<char>());
}

uint32_t readU32LE(const std::vector<uint8_t>& bytes, size_t offset) {
    return  (uint32_t) bytes[offset]
        | (((uint32_t) bytes[offset + 1]) << 8)
        | (((uint32_t) bytes[offset + 2]) << 16)
        | (((uint32_t) bytes[offset + 3]) << 24);
}

uint64_t readU64LE(const std::vector<uint8_t>& bytes, size_t offset) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= ((uint64_t) bytes[offset + (size_t) i]) << (i * 8);
    }
    return v;
}

} // namespace

// --- Header shape ----------------------------------------------------------

TEST(CajetaArchiveTests, emptyArchiveHasCorrectMagicAndVersion) {
    auto path = std::filesystem::temp_directory_path() / "cja_empty.cja";
    {
        CajetaArchive arc("test-empty", "1.0.0", CajetaArchive::Kind::Thin);
        arc.writeTo(path.string());
    }
    auto bytes = readBytes(path.string());
    ASSERT_GE(bytes.size(), (size_t) 32);  // at least the header

    // Magic: "CAJETA01" in bytes 0..7
    EXPECT_EQ(std::string((const char*) bytes.data(), 8), std::string("CAJETA01"));
    // Format version: uint32 LE at offset 8 — current is 1
    EXPECT_EQ(readU32LE(bytes, 8), 1u);
    // Flags: uint32 LE at offset 12 — v1 is all zero (no compression yet)
    EXPECT_EQ(readU32LE(bytes, 12), 0u);
    // Index offset: uint64 LE at offset 16 — v1 is 0 (no trailing index)
    EXPECT_EQ(readU64LE(bytes, 16), 0u);
    // Index length: uint64 LE at offset 24 — v1 is 0
    EXPECT_EQ(readU64LE(bytes, 24), 0u);

    std::filesystem::remove(path);
}

// --- Manifest --------------------------------------------------------------

TEST(CajetaArchiveTests, emptyArchiveHasManifestWithThinKind) {
    auto path = std::filesystem::temp_directory_path() / "cja_manifest.cja";
    {
        CajetaArchive arc("my-lib", "2.0.1", CajetaArchive::Kind::Thin);
        arc.writeTo(path.string());
    }
    auto bytes = readBytes(path.string());

    // After the 32-byte header, the next 8 bytes are the manifest-bytes
    // length (uint64 LE), then the manifest JSON bytes.
    ASSERT_GE(bytes.size(), (size_t) 40);
    uint64_t manifestLen = readU64LE(bytes, 32);
    ASSERT_GT(manifestLen, 0u);
    ASSERT_LE((size_t) (40 + manifestLen), bytes.size());

    std::string manifest((const char*) bytes.data() + 40, manifestLen);
    EXPECT_NE(manifest.find("\"name\":\"my-lib\""), std::string::npos)
        << "manifest = " << manifest;
    EXPECT_NE(manifest.find("\"version\":\"2.0.1\""), std::string::npos);
    EXPECT_NE(manifest.find("\"kind\":\"thin\""), std::string::npos);

    std::filesystem::remove(path);
}

TEST(CajetaArchiveTests, uberArchiveManifestCarriesUberKind) {
    auto path = std::filesystem::temp_directory_path() / "cja_uber.cja";
    {
        CajetaArchive arc("my-app", "0.1.0", CajetaArchive::Kind::Uber);
        arc.writeTo(path.string());
    }
    auto bytes = readBytes(path.string());

    uint64_t manifestLen = readU64LE(bytes, 32);
    std::string manifest((const char*) bytes.data() + 40, manifestLen);
    EXPECT_NE(manifest.find("\"kind\":\"uber\""), std::string::npos);

    std::filesystem::remove(path);
}

// --- Entries ---------------------------------------------------------------

TEST(CajetaArchiveTests, addsBitcodeEntryWithCorrectNameAndPayload) {
    auto path = std::filesystem::temp_directory_path() / "cja_one_entry.cja";
    std::string bitcode = "BCfake_bitcode_payload";   // not real bitcode, just a payload
    {
        CajetaArchive arc("test", "0", CajetaArchive::Kind::Thin);
        CajetaArchiveEntry entry;
        entry.name       = "demo/App.bc";
        entry.originTag  = 0;                                            // user
        entry.kindTag    = CajetaArchive::EntryKind::ClassBitcode;
        entry.data       = std::vector<uint8_t>(bitcode.begin(), bitcode.end());
        arc.addEntry(std::move(entry));
        arc.writeTo(path.string());
    }
    auto bytes = readBytes(path.string());

    // Skip past header (32) + manifest length+bytes
    size_t cursor = 32;
    uint64_t manifestLen = readU64LE(bytes, cursor);
    cursor += 8 + manifestLen;

    // First entry — name_length + name + origin_tag + kind_tag + 2 reserved + data_length + data
    uint32_t nameLen = readU32LE(bytes, cursor);
    cursor += 4;
    EXPECT_EQ(nameLen, (uint32_t) std::strlen("demo/App.bc"));
    std::string name((const char*) bytes.data() + cursor, nameLen);
    EXPECT_EQ(name, "demo/App.bc");
    cursor += nameLen;

    uint8_t originTag = bytes[cursor];      cursor += 1;
    uint8_t kindTag   = bytes[cursor];      cursor += 1;
    cursor += 2;                             // reserved
    uint64_t dataLen = readU64LE(bytes, cursor);
    cursor += 8;
    EXPECT_EQ(originTag, 0u);                            // user
    EXPECT_EQ(kindTag,   0u);                            // class_bitcode
    EXPECT_EQ(dataLen, (uint64_t) bitcode.size());
    std::string data((const char*) bytes.data() + cursor, dataLen);
    EXPECT_EQ(data, bitcode);

    std::filesystem::remove(path);
}

TEST(CajetaArchiveTests, manifestCountsEntries) {
    auto path = std::filesystem::temp_directory_path() / "cja_count.cja";
    {
        CajetaArchive arc("multi", "1", CajetaArchive::Kind::Thin);
        for (int i = 0; i < 3; ++i) {
            CajetaArchiveEntry e;
            e.name = "pkg/Class" + std::to_string(i) + ".bc";
            e.originTag = 0;
            e.kindTag   = CajetaArchive::EntryKind::ClassBitcode;
            e.data      = std::vector<uint8_t>{0x42, 0x43};  // "BC"
            arc.addEntry(std::move(e));
        }
        arc.writeTo(path.string());
    }
    auto bytes = readBytes(path.string());

    uint64_t manifestLen = readU64LE(bytes, 32);
    std::string manifest((const char*) bytes.data() + 40, manifestLen);
    EXPECT_NE(manifest.find("\"entry_count\":3"), std::string::npos)
        << "manifest = " << manifest;

    std::filesystem::remove(path);
}
