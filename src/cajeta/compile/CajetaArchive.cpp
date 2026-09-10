#include "CajetaArchive.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <stdexcept>

#include <zstd.h>

namespace cajeta {

    namespace {
        // Bytes 0..7; an incompatible generation-2 container would say CAJETA02.
        constexpr const char* MAGIC = "CAJETA01";
        constexpr uint32_t FORMAT_VERSION = 1;

        void writeU32LE(std::ostream& out, uint32_t v) {
            char buf[4] = {
                (char) (v & 0xFF),
                (char) ((v >> 8) & 0xFF),
                (char) ((v >> 16) & 0xFF),
                (char) ((v >> 24) & 0xFF),
            };
            out.write(buf, 4);
        }

        void writeU64LE(std::ostream& out, uint64_t v) {
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

        // A manifest string field, or empty; the writer's output is canonical.
        std::string scanManifestString(const std::string& m, const std::string& key) {
            std::string needle = "\"" + key + "\":\"";
            auto pos = m.find(needle);
            if (pos == std::string::npos) return "";
            pos += needle.size();
            auto end = m.find('"', pos);
            if (end == std::string::npos) return "";
            return m.substr(pos, end - pos);
        }

        // The uber-only `deps` array, empty when absent; unnested compact JSON.
        std::vector<CajetaArchive::DepSummary>
        scanManifestDeps(const std::string& m) {
            std::vector<CajetaArchive::DepSummary> out;
            const std::string headerKey = "\"deps\":[";
            auto arrStart = m.find(headerKey);
            if (arrStart == std::string::npos) return out;
            arrStart += headerKey.size();
            auto arrEnd = m.find(']', arrStart);
            if (arrEnd == std::string::npos) return out;
            std::string body = m.substr(arrStart, arrEnd - arrStart);

            std::size_t cursor = 0;
            while (cursor < body.size()) {
                auto objStart = body.find('{', cursor);
                if (objStart == std::string::npos) break;
                auto objEnd = body.find('}', objStart);
                if (objEnd == std::string::npos) break;
                std::string obj = body.substr(objStart, objEnd - objStart + 1);

                CajetaArchive::DepSummary d;
                d.name    = scanManifestString(obj, "name");
                d.version = scanManifestString(obj, "version");
                const std::string cntKey = "\"included_entry_count\":";
                auto cntPos = obj.find(cntKey);
                if (cntPos != std::string::npos) {
                    cntPos += cntKey.size();
                    auto cntEnd = obj.find_first_of(",}", cntPos);
                    if (cntEnd != std::string::npos) {
                        try {
                            d.includedEntryCount = (uint32_t) std::stoul(
                                obj.substr(cntPos, cntEnd - cntPos));
                        } catch (...) { /* leave at 0 */ }
                    }
                }
                if (!d.name.empty()) out.push_back(std::move(d));
                cursor = objEnd + 1;
            }
            return out;
        }

        // The write-few-read-many sweet spot; repack --zstd=<n> exposes 1..22.
        constexpr int ZSTD_DEFAULT_LEVEL = 3;

        // The flags field at offset 12. A compressed section is framed as
        // `uint64 uncompressed_length || zstd_bytes`, and its on-disk length
        // prefix (manifest_len, or an entry's data_length) counts the frame.
        constexpr uint32_t FLAG_MANIFEST_COMPRESSED = 1u << 0;
        constexpr uint32_t FLAG_ENTRIES_COMPRESSED  = 1u << 1;

        // Compresses at `level`, throwing on zstd failure (realistically OOM).
        std::vector<uint8_t> zstdCompress(const uint8_t* src, size_t srcLen,
                                          int level) {
            size_t bound = ZSTD_compressBound(srcLen);
            std::vector<uint8_t> out(bound);
            size_t written = ZSTD_compress(out.data(), bound,
                src, srcLen, level);
            if (ZSTD_isError(written)) {
                throw std::runtime_error(
                    std::string("CajetaArchive: zstd compress failed: ")
                    + ZSTD_getErrorName(written));
            }
            out.resize(written);
            return out;
        }

        // Decompresses, given the uint64 uncompressed size prefixing the frame.
        std::vector<uint8_t> zstdDecompress(const uint8_t* src, size_t srcLen,
                                             uint64_t uncompressedSize) {
            // `uncompressedSize` is untrusted, so it is cross-checked against the
            // frame's own declared size and capped: a decompression bomb would
            // otherwise force the allocation below before anything is decoded.
            static const uint64_t kMaxDecompressed = 1ull << 30;  // 1 GiB
            unsigned long long frameSize =
                ZSTD_getFrameContentSize(src, srcLen);
            if (frameSize == ZSTD_CONTENTSIZE_ERROR
                    || frameSize == ZSTD_CONTENTSIZE_UNKNOWN
                    || frameSize != uncompressedSize
                    || uncompressedSize > kMaxDecompressed) {
                throw std::runtime_error(
                    "CajetaArchive: rejecting zstd section — uncompressed size "
                    "undeclared, mismatched, or over the 1 GiB cap (malformed "
                    "or hostile archive)");
            }
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

    // --- Native artifacts (native-deps subsystem) -------------------------

    void CajetaArchive::addNativeArtifact(const std::string& platform,
                                          const std::string& filename,
                                          std::vector<uint8_t> data) {
        CajetaArchiveEntry e;
        e.name = "native/" + platform + "/" + filename;
        e.originTag = (uint8_t) Origin::User;
        e.kindTag = EntryKind::NativeArtifact;
        e.data = std::move(data);
        addEntry(std::move(e));
    }

    void CajetaArchive::addNativeHeader(const std::string& filename,
                                        std::vector<uint8_t> data) {
        CajetaArchiveEntry e;
        e.name = "native/include/" + filename;
        e.originTag = (uint8_t) Origin::User;
        e.kindTag = EntryKind::NativeArtifact;
        e.data = std::move(data);
        addEntry(std::move(e));
    }

    void CajetaArchive::setNativeLibrariesMeta(std::vector<uint8_t> json) {
        CajetaArchiveEntry e;
        e.name = "native/native-libraries.json";
        e.originTag = (uint8_t) Origin::User;
        e.kindTag = EntryKind::NativeArtifact;
        e.data = std::move(json);
        addEntry(std::move(e));
    }

    std::set<std::string> CajetaArchive::nativePlatforms() const {
        std::set<std::string> out;
        const std::string prefix = "native/";
        for (const auto& e : entries) {
            if (e.kindTag != EntryKind::NativeArtifact) continue;
            if (e.name.rfind(prefix, 0) != 0) continue;
            std::string rest = e.name.substr(prefix.size());
            auto slash = rest.find('/');
            if (slash == std::string::npos) continue;  // e.g. native-libraries.json
            std::string platform = rest.substr(0, slash);
            if (platform == "include") continue;        // headers, not a platform
            out.insert(platform);
        }
        return out;
    }

    std::vector<const CajetaArchiveEntry*>
    CajetaArchive::nativeArtifactsFor(const std::string& platform) const {
        std::vector<const CajetaArchiveEntry*> out;
        const std::string prefix = "native/" + platform + "/";
        for (const auto& e : entries) {
            if (e.kindTag != EntryKind::NativeArtifact) continue;
            if (e.name.rfind(prefix, 0) == 0) out.push_back(&e);
        }
        return out;
    }

    std::vector<const CajetaArchiveEntry*>
    CajetaArchive::nativeHeaders() const {
        std::vector<const CajetaArchiveEntry*> out;
        const std::string prefix = "native/include/";
        for (const auto& e : entries) {
            if (e.kindTag != EntryKind::NativeArtifact) continue;
            if (e.name.rfind(prefix, 0) == 0) out.push_back(&e);
        }
        return out;
    }

    const CajetaArchiveEntry* CajetaArchive::nativeLibrariesMeta() const {
        return findEntry("native/native-libraries.json");
    }

    std::string CajetaArchive::buildManifest() const {
        // Compact JSON by hand: every value is a controlled input (canonical
        // names are ASCII identifiers and dots), so nothing needs escaping.
        std::string kindStr = (kind == Kind::Uber) ? "uber" : "cja";
        std::string out;
        out += "{";
        out += "\"name\":\"" + name + "\"";
        out += ",\"version\":\"" + version + "\"";
        out += ",\"kind\":\"" + kindStr + "\"";
        out += ",\"format_version\":" + std::to_string(FORMAT_VERSION);
        out += ",\"entry_count\":" + std::to_string(entries.size());
        // Uber-only: the classpath archives nested under deps/<name>-<version>/.
        if (kind == Kind::Uber && !deps.empty()) {
            out += ",\"deps\":[";
            for (std::size_t i = 0; i < deps.size(); ++i) {
                if (i > 0) out += ",";
                out += "{\"name\":\"" + deps[i].name + "\"";
                out += ",\"version\":\"" + deps[i].version + "\"";
                out += ",\"included_entry_count\":"
                    + std::to_string(deps[i].includedEntryCount);
                out += "}";
            }
            out += "]";
        }
        out += "}";
        return out;
    }

    const CajetaArchiveEntry* CajetaArchive::findEntry(const std::string& name) const {
        if (nameIndex.empty() && !entries.empty()) {
            // First-wins on duplicates, matching the writer's dedupe pass.
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
        writeToStream(out);
        if (!out) {
            throw std::runtime_error("CajetaArchive: write failure: " + path);
        }
    }

    void CajetaArchive::writeToStream(std::ostream& sink) {
        // Buffered because the header patch seeks back and a pipe cannot.
        std::ostringstream buf(std::ios::binary);
        std::ostream& out = buf;

        bool compressed = (compression == Compression::Zstd);
        uint32_t flags = 0;
        if (compressed) {
            flags |= FLAG_MANIFEST_COMPRESSED | FLAG_ENTRIES_COMPRESSED;
        }

        // ---- Header (32 bytes) ----
        // index_offset / index_length are patched once the index is appended.
        out.write(MAGIC, 8);
        writeU32LE(out, FORMAT_VERSION);
        writeU32LE(out, flags);
        writeU64LE(out, /*index_offset=*/0);     // patched below
        writeU64LE(out, /*index_length=*/0);     // patched below

        // ---- Manifest ----
        std::string manifest = buildManifest();
        if (compressed) {
            // manifest_len counts the whole frame: 8 + zstd_bytes.size().
            auto comp = zstdCompress(
                (const uint8_t*) manifest.data(), manifest.size(),
                compressionLevel);
            writeU64LE(out, (uint64_t) (8 + comp.size()));
            writeU64LE(out, (uint64_t) manifest.size());
            out.write((const char*) comp.data(), (std::streamsize) comp.size());
        } else {
            writeU64LE(out, (uint64_t) manifest.size());
            out.write(manifest.data(), (std::streamsize) manifest.size());
        }

        // ---- Entries ----
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
                auto comp = zstdCompress(e.data.data(), e.data.size(),
                    compressionLevel);
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
        // uint32 entry_count, then per entry: uint32 name_length, the name,
        // uint64 entry_offset (at its name_length field) and uint64 entry_size
        // (its whole on-disk span). Always written; readers test index_offset.
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

        // index_offset is at byte 16, index_length at byte 24.
        out.seekp(16);
        writeU64LE(out, indexOffset);
        writeU64LE(out, indexLength);

        if (!out) {
            throw std::runtime_error("CajetaArchive: stringstream write failure");
        }
        std::string archive = buf.str();
        sink.write(archive.data(), (std::streamsize) archive.size());
    }

    CajetaArchive CajetaArchive::readFrom(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::runtime_error("CajetaArchive: cannot open: " + path);
        }
        std::vector<uint8_t> bytes(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
        return readFromBytes(bytes, path);
    }

    CajetaArchive CajetaArchive::readFromBytes(
            const std::vector<uint8_t>& bytes, const std::string& sourceName) {
        // `sourceName` is opaque here, and appears in the diagnostics below.
        const std::string& path = sourceName;

        // A security surface: classpath archives come from outside the build.
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
        // An unknown bit is a later format: reject rather than misparse it.
        constexpr uint32_t KNOWN_FLAGS =
            FLAG_MANIFEST_COMPRESSED | FLAG_ENTRIES_COMPRESSED;
        if ((flags & ~KNOWN_FLAGS) != 0) {
            throw std::runtime_error(
                "CajetaArchive: " + path + " uses unknown flags=0x"
                + std::to_string(flags));
        }
        bool manifestCompressed = (flags & FLAG_MANIFEST_COMPRESSED) != 0;
        bool entriesCompressed  = (flags & FLAG_ENTRIES_COMPRESSED) != 0;

        // Manifest: uint64 on-disk length, then that many bytes of JSON frame.
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
        // Entries end at the trailing index, or at EOF for a pre-index archive.
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
        // Anything but "uber" reads as cja, including the legacy "thin" tag.
        Kind archKind = (kindStr == "uber") ? Kind::Uber : Kind::Cja;

        CajetaArchive arc(archiveName, archiveVer, archKind);
        arc.setSourceArchiveName(archiveName);
        arc.setRawManifest(manifest);
        if (archKind == Kind::Uber) {
            arc.setDeps(scanManifestDeps(manifest));
        }

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
