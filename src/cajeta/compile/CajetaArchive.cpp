#include "CajetaArchive.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include <zstd.h>

namespace cajeta {

    namespace {
        // Magic header — bytes 0..7. The "01" suffix is a human-readable
        // hint that this is format generation 1; an incompatible
        // generation-2 container would use "CAJETA02".
        constexpr const char* MAGIC = "CAJETA01";
        constexpr uint32_t FORMAT_VERSION = 1;

        void writeU32LE(std::ofstream& out, uint32_t v) {
            char buf[4] = {
                (char) (v & 0xFF),
                (char) ((v >> 8) & 0xFF),
                (char) ((v >> 16) & 0xFF),
                (char) ((v >> 24) & 0xFF),
            };
            out.write(buf, 4);
        }

        void writeU64LE(std::ofstream& out, uint64_t v) {
            char buf[8];
            for (int i = 0; i < 8; ++i) {
                buf[i] = (char) ((v >> (i * 8)) & 0xFF);
            }
            out.write(buf, 8);
        }

        uint32_t readU32LE(const uint8_t* p) {
            return  (uint32_t) p[0]
                | (((uint32_t) p[1]) << 8)
                | (((uint32_t) p[2]) << 16)
                | (((uint32_t) p[3]) << 24);
        }

        uint64_t readU64LE(const uint8_t* p) {
            uint64_t v = 0;
            for (int i = 0; i < 8; ++i) {
                v |= ((uint64_t) p[i]) << (i * 8);
            }
            return v;
        }

        // Scan the manifest JSON for a string field. Pure substring
        // search — the manifest writer keeps its output canonical
        // (no whitespace, no escapes for the controlled set of keys)
        // so we don't need a real JSON parser here. Returns empty
        // string on miss.
        std::string scanManifestString(const std::string& m, const std::string& key) {
            std::string needle = "\"" + key + "\":\"";
            auto pos = m.find(needle);
            if (pos == std::string::npos) return "";
            pos += needle.size();
            auto end = m.find('"', pos);
            if (end == std::string::npos) return "";
            return m.substr(pos, end - pos);
        }

        // zstd compression level. 3 is the default — fast encoder, very
        // fast decoder, decent ratio. Levels 1-3 are speed-optimized;
        // 19-22 are ratio-optimized. The .cja write-few-read-many
        // workload favors decompression speed (libraries written once,
        // read by every consumer that links them), so 3 is the sweet
        // spot.
        constexpr int ZSTD_COMPRESS_LEVEL = 3;

        // Header flag bits — written into the 4-byte flags field at
        // offset 12. Bit 0 marks the manifest section as zstd-
        // compressed; bit 1 marks every entry's data block. Both
        // sections, when compressed, are framed as:
        //   uint64 uncompressed_length || zstd_compressed_bytes
        // The on-disk length-prefix (manifest_len for the manifest,
        // data_length per entry) describes the COMPRESSED on-disk size.
        constexpr uint32_t FLAG_MANIFEST_COMPRESSED = 1u << 0;
        constexpr uint32_t FLAG_ENTRIES_COMPRESSED  = 1u << 1;

        // Compress raw bytes with zstd. Returns the compressed buffer.
        // Throws on zstd failure (malformed input is impossible since we
        // pass valid bytes; out-of-memory is the realistic case).
        std::vector<uint8_t> zstdCompress(const uint8_t* src, size_t srcLen) {
            size_t bound = ZSTD_compressBound(srcLen);
            std::vector<uint8_t> out(bound);
            size_t written = ZSTD_compress(out.data(), bound,
                src, srcLen, ZSTD_COMPRESS_LEVEL);
            if (ZSTD_isError(written)) {
                throw std::runtime_error(
                    std::string("CajetaArchive: zstd compress failed: ")
                    + ZSTD_getErrorName(written));
            }
            out.resize(written);
            return out;
        }

        // Decompress zstd bytes given the known uncompressed size (the
        // .cja format stores it as a uint64 prefix in each compressed
        // section).
        std::vector<uint8_t> zstdDecompress(const uint8_t* src, size_t srcLen,
                                             uint64_t uncompressedSize) {
            std::vector<uint8_t> out((size_t) uncompressedSize);
            size_t produced = ZSTD_decompress(out.data(), out.size(),
                src, srcLen);
            if (ZSTD_isError(produced)) {
                throw std::runtime_error(
                    std::string("CajetaArchive: zstd decompress failed: ")
                    + ZSTD_getErrorName(produced));
            }
            if (produced != (size_t) uncompressedSize) {
                throw std::runtime_error(
                    "CajetaArchive: zstd decompress produced "
                    + std::to_string(produced) + " bytes, expected "
                    + std::to_string(uncompressedSize));
            }
            return out;
        }
    }

    CajetaArchive::CajetaArchive(std::string name, std::string version, Kind kind)
        : name(std::move(name)), version(std::move(version)), kind(kind) {
    }

    void CajetaArchive::addEntry(CajetaArchiveEntry entry) {
        entries.push_back(std::move(entry));
    }

    std::string CajetaArchive::buildManifest() const {
        // Minimal compact JSON — no library dependency, no escapes needed
        // because name/version/kind are all controlled inputs and the
        // entry_count is an integer. If user names ever carry a `"` we'd
        // need a real escaper; for now the cajeta canonical-name format
        // is restricted to ASCII identifier chars + dots.
        std::string kindStr = (kind == Kind::Uber) ? "uber" : "thin";
        std::string out;
        out += "{";
        out += "\"name\":\"" + name + "\"";
        out += ",\"version\":\"" + version + "\"";
        out += ",\"kind\":\"" + kindStr + "\"";
        out += ",\"format_version\":" + std::to_string(FORMAT_VERSION);
        out += ",\"entry_count\":" + std::to_string(entries.size());
        out += "}";
        return out;
    }

    const CajetaArchiveEntry* CajetaArchive::findEntry(const std::string& name) const {
        if (nameIndex.empty() && !entries.empty()) {
            // Build lazily on first call. First-wins for duplicates so the
            // semantic matches what the writer's dedupe pass produced.
            for (std::size_t i = 0; i < entries.size(); ++i) {
                nameIndex.emplace(entries[i].name, i);
            }
        }
        auto it = nameIndex.find(name);
        if (it == nameIndex.end()) return nullptr;
        return &entries[it->second];
    }

    void CajetaArchive::writeTo(const std::string& path) {
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("CajetaArchive: cannot open output: " + path);
        }

        bool compressed = (compression == Compression::Zstd);
        uint32_t flags = 0;
        if (compressed) {
            flags |= FLAG_MANIFEST_COMPRESSED | FLAG_ENTRIES_COMPRESSED;
        }

        // ---- Header (32 bytes) ----
        // index_offset / index_length are placeholders; we patch them
        // post-write once the index has been appended and we know its
        // actual offset + length.
        out.write(MAGIC, 8);
        writeU32LE(out, FORMAT_VERSION);
        writeU32LE(out, flags);
        writeU64LE(out, /*index_offset=*/0);     // patched below
        writeU64LE(out, /*index_length=*/0);     // patched below

        // ---- Manifest ----
        std::string manifest = buildManifest();
        if (compressed) {
            // Frame: uint64 uncompressed_len || zstd_compressed_bytes.
            // The manifest_len that follows in the format gives the
            // on-disk size, which here is 8 + zstd_bytes.size().
            auto comp = zstdCompress(
                (const uint8_t*) manifest.data(), manifest.size());
            writeU64LE(out, (uint64_t) (8 + comp.size()));
            writeU64LE(out, (uint64_t) manifest.size());
            out.write((const char*) comp.data(), (std::streamsize) comp.size());
        } else {
            writeU64LE(out, (uint64_t) manifest.size());
            out.write(manifest.data(), (std::streamsize) manifest.size());
        }

        // ---- Entries ----
        // Track each entry's start offset for the trailing index.
        std::vector<uint64_t> entryOffsets;
        std::vector<uint64_t> entryOnDiskSizes;
        entryOffsets.reserve(entries.size());
        entryOnDiskSizes.reserve(entries.size());
        for (const auto& e : entries) {
            uint64_t startOffset = (uint64_t) out.tellp();
            entryOffsets.push_back(startOffset);

            writeU32LE(out, (uint32_t) e.name.size());
            out.write(e.name.data(), (std::streamsize) e.name.size());

            char originByte = (char) e.originTag;
            char kindByte   = (char) e.kindTag;
            char reserved[2] = {0, 0};
            out.write(&originByte, 1);
            out.write(&kindByte,   1);
            out.write(reserved,    2);

            if (compressed) {
                auto comp = zstdCompress(e.data.data(), e.data.size());
                writeU64LE(out, (uint64_t) (8 + comp.size()));
                writeU64LE(out, (uint64_t) e.data.size());
                out.write((const char*) comp.data(), (std::streamsize) comp.size());
            } else {
                writeU64LE(out, (uint64_t) e.data.size());
                out.write((const char*) e.data.data(),
                    (std::streamsize) e.data.size());
            }

            entryOnDiskSizes.push_back((uint64_t) out.tellp() - startOffset);
        }

        // ---- Trailing index ----
        // Format:
        //   uint32 entry_count
        //   for each entry:
        //     uint32 name_length
        //     bytes  name
        //     uint64 entry_offset    (start of name_length field)
        //     uint64 entry_size      (total on-disk bytes including
        //                             name+tags+data+framing)
        // Always written; readers consult `index_offset != 0` in the
        // header to decide whether to use it for random access. Cost is
        // small (~24 bytes per entry + names); benefit is O(1) lookup
        // by name across an arbitrarily large archive.
        uint64_t indexOffset = (uint64_t) out.tellp();
        writeU32LE(out, (uint32_t) entries.size());
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            writeU32LE(out, (uint32_t) e.name.size());
            out.write(e.name.data(), (std::streamsize) e.name.size());
            writeU64LE(out, entryOffsets[i]);
            writeU64LE(out, entryOnDiskSizes[i]);
        }
        uint64_t indexLength = (uint64_t) out.tellp() - indexOffset;

        // Patch header's index_offset (byte 16) and index_length (byte 24).
        out.seekp(16);
        writeU64LE(out, indexOffset);
        writeU64LE(out, indexLength);

        if (!out) {
            throw std::runtime_error("CajetaArchive: write failure: " + path);
        }
    }

    CajetaArchive CajetaArchive::readFrom(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("CajetaArchive: cannot open: " + path);
        }
        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());

        // Header is 32 bytes. Bail loudly on anything that doesn't look
        // like a cajeta archive — the reader is a security surface
        // (classpath-loaded archives come from outside the build),
        // so the diagnostics need to be specific.
        if (bytes.size() < 32) {
            throw std::runtime_error(
                "CajetaArchive: " + path + " is too short to be a .cja file");
        }
        if (std::memcmp(bytes.data(), "CAJETA01", 8) != 0) {
            throw std::runtime_error(
                "CajetaArchive: " + path + " bad magic (expected CAJETA01)");
        }
        uint32_t version    = readU32LE(bytes.data() + 8);
        uint32_t flags      = readU32LE(bytes.data() + 12);
        uint64_t indexOff   = readU64LE(bytes.data() + 16);
        uint64_t indexLen   = readU64LE(bytes.data() + 24);
        (void) indexOff;
        (void) indexLen;
        if (version != 1) {
            throw std::runtime_error(
                "CajetaArchive: " + path + " unsupported format version "
                + std::to_string(version));
        }
        // Recognized flag bits: bit 0 (manifest_compressed) + bit 1
        // (entries_compressed). Other bits would be a future format
        // extension we don't yet understand — reject loudly so a
        // future-shaped archive can't silently misparse through this
        // reader.
        constexpr uint32_t KNOWN_FLAGS =
            FLAG_MANIFEST_COMPRESSED | FLAG_ENTRIES_COMPRESSED;
        if ((flags & ~KNOWN_FLAGS) != 0) {
            throw std::runtime_error(
                "CajetaArchive: " + path + " uses unknown flags=0x"
                + std::to_string(flags));
        }
        bool manifestCompressed = (flags & FLAG_MANIFEST_COMPRESSED) != 0;
        bool entriesCompressed  = (flags & FLAG_ENTRIES_COMPRESSED) != 0;

        // Manifest: uint64 on-disk length-prefixed UTF-8 JSON. When
        // compressed, the on-disk block is uint64 uncompressed_len ||
        // zstd_bytes (total length = on-disk length).
        if (bytes.size() < 40) {
            throw std::runtime_error("CajetaArchive: " + path + " truncated at manifest");
        }
        uint64_t manifestOnDisk = readU64LE(bytes.data() + 32);
        if (manifestOnDisk > bytes.size() - 40) {
            throw std::runtime_error("CajetaArchive: " + path + " manifest length out of range");
        }
        std::string manifest;
        if (manifestCompressed) {
            if (manifestOnDisk < 8) {
                throw std::runtime_error(
                    "CajetaArchive: " + path + " compressed manifest too short");
            }
            uint64_t uncompressedLen = readU64LE(bytes.data() + 40);
            auto decompressed = zstdDecompress(
                bytes.data() + 48,
                (size_t) (manifestOnDisk - 8),
                uncompressedLen);
            manifest.assign((const char*) decompressed.data(),
                decompressed.size());
        } else {
            manifest.assign(
                (const char*) bytes.data() + 40,
                (size_t) manifestOnDisk);
        }
        size_t cursor = 40 + (size_t) manifestOnDisk;
        // Entries end at the trailing index (when present) or at EOF.
        // Earlier writes (v1.0 before the trailing index landed) set
        // both indexOff and indexLen to 0 — in that case fall through
        // to EOF.
        size_t entriesEnd = (indexOff != 0)
            ? (size_t) indexOff
            : bytes.size();
        if (entriesEnd > bytes.size()) {
            throw std::runtime_error(
                "CajetaArchive: " + path + " index_offset past EOF");
        }

        std::string archiveName = scanManifestString(manifest, "name");
        std::string archiveVer  = scanManifestString(manifest, "version");
        std::string kindStr     = scanManifestString(manifest, "kind");
        Kind archKind = (kindStr == "uber") ? Kind::Uber : Kind::Thin;

        CajetaArchive arc(archiveName, archiveVer, archKind);
        arc.setSourceArchiveName(archiveName);

        // Entries — sequential read until the entries-section end
        // (start of trailing index, or EOF for pre-index archives).
        while (cursor < entriesEnd) {
            if (bytes.size() - cursor < 4) {
                throw std::runtime_error(
                    "CajetaArchive: " + path + " truncated entry name length");
            }
            uint32_t nameLen = readU32LE(bytes.data() + cursor);
            cursor += 4;
            if (bytes.size() - cursor < nameLen) {
                throw std::runtime_error(
                    "CajetaArchive: " + path + " truncated entry name");
            }
            std::string name(
                (const char*) bytes.data() + cursor, nameLen);
            cursor += nameLen;

            if (bytes.size() - cursor < 12) {
                throw std::runtime_error(
                    "CajetaArchive: " + path + " truncated entry header");
            }
            uint8_t originTag = bytes[cursor];
            uint8_t kindTag   = bytes[cursor + 1];
            // skip 2 reserved bytes
            cursor += 4;
            uint64_t dataOnDisk = readU64LE(bytes.data() + cursor);
            cursor += 8;
            if (bytes.size() - cursor < dataOnDisk) {
                throw std::runtime_error(
                    "CajetaArchive: " + path + " truncated entry data");
            }

            CajetaArchiveEntry entry;
            entry.name      = std::move(name);
            entry.originTag = originTag;
            entry.kindTag   = (EntryKind) kindTag;
            if (entriesCompressed) {
                if (dataOnDisk < 8) {
                    throw std::runtime_error(
                        "CajetaArchive: " + path + " compressed entry too short");
                }
                uint64_t uncompressedLen = readU64LE(bytes.data() + cursor);
                entry.data = zstdDecompress(
                    bytes.data() + cursor + 8,
                    (size_t) (dataOnDisk - 8),
                    uncompressedLen);
            } else {
                entry.data.assign(
                    bytes.data() + cursor,
                    bytes.data() + cursor + dataOnDisk);
            }
            cursor += (size_t) dataOnDisk;

            arc.addEntry(std::move(entry));
        }

        return arc;
    }

} // namespace cajeta
