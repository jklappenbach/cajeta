#include "CajetaArchive.h"

#include <fstream>
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

} // namespace cajeta
