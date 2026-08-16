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
#include <filesystem>
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


// --- Manifest --------------------------------------------------------------



// --- Entries ---------------------------------------------------------------


// --- Reader roundtrip -----------------------------------------------------






// --- Compression -----------------------------------------------------------

TEST(CajetaArchiveTests, compressedArchiveSetsFlagsBit0And1) {
    auto path = std::filesystem::temp_directory_path() / "cja_zstd_flags.cja";
    {
        CajetaArchive arc("zstd", "1", CajetaArchive::Kind::Cja);
        arc.setCompression(CajetaArchive::Compression::Zstd);
        // Need at least one entry so flag bit 1 is meaningful (the
        // writer sets the bit unconditionally when compression is on,
        // but the assertion below also reads the manifest's flag bit 0).
        CajetaArchiveEntry e;
        e.name = "a/B.bc";
        e.originTag = 0;
        e.kindTag   = CajetaArchive::EntryKind::ClassBitcode;
        e.data.assign(1024, 0x42);  // 1KB of 'B' — highly compressible
        arc.addEntry(std::move(e));
        arc.writeTo(path.string());
    }
    auto bytes = readBytes(path.string());
    ASSERT_GE(bytes.size(), (size_t) 32);
    uint32_t flags = readU32LE(bytes, 12);
    EXPECT_TRUE(flags & 0x1) << "manifest_compressed flag should be set";
    EXPECT_TRUE(flags & 0x2) << "entries_compressed flag should be set";
    std::filesystem::remove(path);
}

TEST(CajetaArchiveTests, compressedRoundtripPreservesEntries) {
    auto path = std::filesystem::temp_directory_path() / "cja_zstd_rt.cja";
    std::vector<uint8_t> payload(2048);
    // Deterministic but non-trivial pattern so compression has something
    // to do.
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = (uint8_t) ((i * 7 + 11) & 0xFF);
    }
    {
        CajetaArchive arc("rt-zstd", "1", CajetaArchive::Kind::Uber);
        arc.setCompression(CajetaArchive::Compression::Zstd);
        CajetaArchiveEntry e1{"x/A.bc", 0,
            CajetaArchive::EntryKind::ClassBitcode, payload};
        CajetaArchiveEntry e2{"x/y/B.bc", 2,
            CajetaArchive::EntryKind::ClassBitcode,
            std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}};
        arc.addEntry(std::move(e1));
        arc.addEntry(std::move(e2));
        arc.writeTo(path.string());
    }
    auto loaded = CajetaArchive::readFrom(path.string());
    EXPECT_EQ(loaded.getName(),    "rt-zstd");
    EXPECT_EQ(loaded.getKind(),    CajetaArchive::Kind::Uber);
    ASSERT_EQ(loaded.getEntries().size(), 2u);
    EXPECT_EQ(loaded.getEntries()[0].name, "x/A.bc");
    EXPECT_EQ(loaded.getEntries()[0].data, payload);
    EXPECT_EQ(loaded.getEntries()[1].name, "x/y/B.bc");
    EXPECT_EQ(loaded.getEntries()[1].data,
        (std::vector<uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}));
    std::filesystem::remove(path);
}

TEST(CajetaArchiveTests, compressedArchiveIsSmallerThanUncompressed) {
    auto pathRaw  = std::filesystem::temp_directory_path() / "cja_size_raw.cja";
    auto pathZstd = std::filesystem::temp_directory_path() / "cja_size_zstd.cja";
    // Highly compressible payload — a 16 KB run of the same byte.
    std::vector<uint8_t> payload(16 * 1024, 0x7A);
    auto buildAndWrite = [&](CajetaArchive::Compression c,
                              const std::filesystem::path& path) {
        CajetaArchive arc("sz", "1", CajetaArchive::Kind::Cja);
        arc.setCompression(c);
        CajetaArchiveEntry e{"p/Big.bc", 0,
            CajetaArchive::EntryKind::ClassBitcode, payload};
        arc.addEntry(std::move(e));
        arc.writeTo(path.string());
    };
    buildAndWrite(CajetaArchive::Compression::None, pathRaw);
    buildAndWrite(CajetaArchive::Compression::Zstd, pathZstd);

    auto rawSize  = std::filesystem::file_size(pathRaw);
    auto zstdSize = std::filesystem::file_size(pathZstd);
    EXPECT_LT(zstdSize, rawSize)
        << "expected zstd to compress the 16 KB run; "
        << "raw=" << rawSize << " zstd=" << zstdSize;
    // 16 KB of the same byte should compress to a few hundred bytes
    // at most; the file should be < 1 KB total including the header
    // and manifest framing.
    EXPECT_LT(zstdSize, (uintmax_t) 1024)
        << "zstd archive of a single-byte run should be tiny; got "
        << zstdSize << " bytes";

    std::filesystem::remove(pathRaw);
    std::filesystem::remove(pathZstd);
}

TEST(CajetaArchiveTests, readerHandlesBothCompressedAndUncompressed) {
    // The same payload via both writer paths should produce identical
    // reader outputs (modulo the source archive's compression flag,
    // which the reader doesn't track post-decompress).
    auto pathRaw  = std::filesystem::temp_directory_path() / "cja_both_raw.cja";
    auto pathZstd = std::filesystem::temp_directory_path() / "cja_both_zstd.cja";
    std::vector<uint8_t> payload{0x11, 0x22, 0x33, 0x44, 0x55};

    auto build = [&](CajetaArchive::Compression c,
                      const std::filesystem::path& path) {
        CajetaArchive arc("both", "1", CajetaArchive::Kind::Cja);
        arc.setCompression(c);
        CajetaArchiveEntry e{"a/B.bc", 0,
            CajetaArchive::EntryKind::ClassBitcode, payload};
        arc.addEntry(std::move(e));
        arc.writeTo(path.string());
    };
    build(CajetaArchive::Compression::None, pathRaw);
    build(CajetaArchive::Compression::Zstd, pathZstd);

    auto loadedRaw  = CajetaArchive::readFrom(pathRaw.string());
    auto loadedZstd = CajetaArchive::readFrom(pathZstd.string());
    ASSERT_EQ(loadedRaw.getEntries().size(),  1u);
    ASSERT_EQ(loadedZstd.getEntries().size(), 1u);
    EXPECT_EQ(loadedRaw.getEntries()[0].data,  payload);
    EXPECT_EQ(loadedZstd.getEntries()[0].data, payload);

    std::filesystem::remove(pathRaw);
    std::filesystem::remove(pathZstd);
}

// --- Trailing random-access index -----------------------------------------

TEST(CajetaArchiveTests, writerWritesNonZeroIndexOffsetAndLength) {
    auto path = std::filesystem::temp_directory_path() / "cja_idx_basic.cja";
    {
        CajetaArchive arc("idx", "1", CajetaArchive::Kind::Cja);
        arc.setCompression(CajetaArchive::Compression::None);
        CajetaArchiveEntry e{"a/B.bc", 0,
            CajetaArchive::EntryKind::ClassBitcode,
            std::vector<uint8_t>{0xAA, 0xBB}};
        arc.addEntry(std::move(e));
        arc.writeTo(path.string());
    }
    auto bytes = readBytes(path.string());
    ASSERT_GE(bytes.size(), (size_t) 32);
    uint64_t indexOffset = readU64LE(bytes, 16);
    uint64_t indexLength = readU64LE(bytes, 24);
    EXPECT_GT(indexOffset, (uint64_t) 32);                  // past header
    EXPECT_GT(indexLength, (uint64_t) 0);
    EXPECT_EQ(indexOffset + indexLength, (uint64_t) bytes.size())
        << "trailing index must extend to EOF";

    // The index block starts with a uint32 entry_count.
    ASSERT_LE(indexOffset + 4, bytes.size());
    uint32_t entryCount = readU32LE(bytes, (size_t) indexOffset);
    EXPECT_EQ(entryCount, 1u);

    std::filesystem::remove(path);
}

TEST(CajetaArchiveTests, findEntryFindsByName) {
    auto path = std::filesystem::temp_directory_path() / "cja_idx_find.cja";
    std::vector<uint8_t> payload1{0x11, 0x22};
    std::vector<uint8_t> payload2{0x33, 0x44, 0x55};
    {
        CajetaArchive arc("find", "1", CajetaArchive::Kind::Cja);
        arc.addEntry({"x/A.bc", 0,
            CajetaArchive::EntryKind::ClassBitcode, payload1});
        arc.addEntry({"x/y/B.bc", 0,
            CajetaArchive::EntryKind::ClassBitcode, payload2});
        arc.writeTo(path.string());
    }
    auto loaded = CajetaArchive::readFrom(path.string());
    const auto* a = loaded.findEntry("x/A.bc");
    const auto* b = loaded.findEntry("x/y/B.bc");
    const auto* missing = loaded.findEntry("nope/Missing.bc");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(missing, nullptr);
    EXPECT_EQ(a->data, payload1);
    EXPECT_EQ(b->data, payload2);
    std::filesystem::remove(path);
}

TEST(CajetaArchiveTests, indexSurvivesCompression) {
    // The index follows the entries regardless of whether they're
    // compressed — the offsets it records are on-disk byte positions,
    // not logical entry indices.
    auto path = std::filesystem::temp_directory_path() / "cja_idx_zstd.cja";
    {
        CajetaArchive arc("idx-zstd", "1", CajetaArchive::Kind::Cja);
        arc.setCompression(CajetaArchive::Compression::Zstd);
        arc.addEntry({"a.bc", 0,
            CajetaArchive::EntryKind::ClassBitcode,
            std::vector<uint8_t>(2048, 0x77)});
        arc.addEntry({"b.bc", 0,
            CajetaArchive::EntryKind::ClassBitcode,
            std::vector<uint8_t>{0x12, 0x34}});
        arc.writeTo(path.string());
    }
    auto bytes = readBytes(path.string());
    uint64_t indexOffset = readU64LE(bytes, 16);
    uint64_t indexLength = readU64LE(bytes, 24);
    EXPECT_GT(indexOffset, (uint64_t) 32);
    EXPECT_GT(indexLength, (uint64_t) 0);

    auto loaded = CajetaArchive::readFrom(path.string());
    ASSERT_EQ(loaded.getEntries().size(), 2u);
    const auto* a = loaded.findEntry("a.bc");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a->data.size(), (size_t) 2048);

    std::filesystem::remove(path);
}

