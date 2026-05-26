#include "CajetaArchive.h"

#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

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

    void CajetaArchive::writeTo(const std::string& path) {
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("CajetaArchive: cannot open output: " + path);
        }

        // ---- Header (32 bytes) ----
        out.write(MAGIC, 8);
        writeU32LE(out, FORMAT_VERSION);
        writeU32LE(out, /*flags=*/0);          // v1: no compression, no trailing index
        writeU64LE(out, /*index_offset=*/0);
        writeU64LE(out, /*index_length=*/0);

        // ---- Manifest ----
        std::string manifest = buildManifest();
        writeU64LE(out, (uint64_t) manifest.size());
        out.write(manifest.data(), (std::streamsize) manifest.size());

        // ---- Entries ----
        for (const auto& e : entries) {
            writeU32LE(out, (uint32_t) e.name.size());
            out.write(e.name.data(), (std::streamsize) e.name.size());

            char originByte = (char) e.originTag;
            char kindByte   = (char) e.kindTag;
            char reserved[2] = {0, 0};
            out.write(&originByte, 1);
            out.write(&kindByte,   1);
            out.write(reserved,    2);

            writeU64LE(out, (uint64_t) e.data.size());
            out.write((const char*) e.data.data(), (std::streamsize) e.data.size());
        }

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
        // v1 only supports flags == 0 (uncompressed, no trailing index).
        // zstd / trailing-index land later via flag bits 0 / 1 / 2 —
        // this reject keeps a future-shaped archive from silently
        // misparsing through the v1 reader.
        if (flags != 0) {
            throw std::runtime_error(
                "CajetaArchive: " + path + " uses flags=0x"
                + std::to_string(flags)
                + " which this reader doesn't support yet");
        }

        // Manifest: uint64 length-prefixed UTF-8 JSON.
        if (bytes.size() < 40) {
            throw std::runtime_error("CajetaArchive: " + path + " truncated at manifest");
        }
        uint64_t manifestLen = readU64LE(bytes.data() + 32);
        if (manifestLen > bytes.size() - 40) {
            throw std::runtime_error("CajetaArchive: " + path + " manifest length out of range");
        }
        std::string manifest(
            (const char*) bytes.data() + 40, manifestLen);
        size_t cursor = 40 + (size_t) manifestLen;

        std::string archiveName = scanManifestString(manifest, "name");
        std::string archiveVer  = scanManifestString(manifest, "version");
        std::string kindStr     = scanManifestString(manifest, "kind");
        Kind archKind = (kindStr == "uber") ? Kind::Uber : Kind::Thin;

        CajetaArchive arc(archiveName, archiveVer, archKind);
        arc.setSourceArchiveName(archiveName);

        // Entries — sequential read until end of file. v1 has no
        // trailing index so we scan linearly.
        while (cursor < bytes.size()) {
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
            uint64_t dataLen = readU64LE(bytes.data() + cursor);
            cursor += 8;
            if (bytes.size() - cursor < dataLen) {
                throw std::runtime_error(
                    "CajetaArchive: " + path + " truncated entry data");
            }

            CajetaArchiveEntry entry;
            entry.name      = std::move(name);
            entry.originTag = originTag;
            entry.kindTag   = (EntryKind) kindTag;
            entry.data.assign(
                bytes.data() + cursor,
                bytes.data() + cursor + dataLen);
            cursor += (size_t) dataLen;

            arc.addEntry(std::move(entry));
        }

        return arc;
    }

} // namespace cajeta
