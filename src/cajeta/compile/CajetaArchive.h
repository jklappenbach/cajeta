// `.cja` (Cajeta ARchive) writer — v1 minimum-viable implementation
// of the container format spelled out in cajeta-docs/Compilation.md
// § Archive format § Minimum-viable v1.
//
// Layout produced by writeTo():
//   [ 32-byte header ]
//     8  bytes  magic         "CAJETA01"
//     4  bytes  format_version uint32 LE (currently 1)
//     4  bytes  flags          uint32 LE (v1 always 0 — no compression)
//     8  bytes  index_offset   uint64 LE (v1 always 0 — no trailing index)
//     8  bytes  index_length   uint64 LE (v1 always 0)
//   [ manifest_length ]      uint64 LE
//   [ manifest_bytes ]       raw UTF-8 JSON
//   [ entry_1 ]              see CajetaArchiveEntry::serialize
//   [ entry_2 ]
//   ...
//
// Used by Compiler::emitForModule when the user passes
// --emit=archive (thin form) or --emit=uber. Uber form populates the
// manifest with an "origins" map and adds dep-archive entries; the
// container shape is otherwise identical.
//
// v1 omits zstd compression, the trailing index, the resources block,
// and the runtime-bitcode block. Adding them later only flips header
// flag bits — the format-version field stays at 1 because additions
// land via flags + manifest fields, not version bumps.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace cajeta {

    class CajetaArchive {
    public:
        // Thin vs uber distinction. Stored in the manifest's "kind"
        // field; the on-disk container is identical otherwise.
        enum class Kind {
            Thin,
            Uber,
        };

        // Entry kind tags written as a single byte on each entry.
        enum class EntryKind : uint8_t {
            ClassBitcode  = 0,   // LLVM bitcode for one cajeta class
            Resource      = 1,   // raw bytes (templates, configs, etc.)
            RuntimeBitcode = 2,  // C runtime bitcode (for --emit=exe inputs)
        };

        // Origin tags written as a single byte on each entry. Lets uber
        // archives' "origins" manifest map track where each entry
        // originated. Thin archives use Origin::User for user code and
        // Origin::Stdlib for parsed-stdlib classes.
        enum class Origin : uint8_t {
            User    = 0,
            Stdlib  = 1,
            Dependency = 2,
        };

        // Compression algorithm for both the manifest and entry payloads.
        // None keeps writes raw (header flags 0, smaller writer, larger
        // file). Zstd compresses both with zstd at level 3 — fast encoder,
        // very fast decoder, decent ratio. The reader auto-detects via
        // header flag bits 0 (manifest) and 1 (entries) and decompresses
        // transparently regardless of how the writer was configured.
        enum class Compression {
            None,
            Zstd,
        };

        CajetaArchive(std::string name, std::string version, Kind kind);

        // Choose the compression algorithm for the writer. Default: Zstd —
        // user-facing archives get small files out of the box. Tests that
        // probe raw header bytes explicitly call setCompression(None) so
        // their byte-offset assertions stay stable.
        void setCompression(Compression c) { compression = c; }
        Compression getCompression() const { return compression; }

        // Add one entry. Takes ownership of the entry's data vector
        // (move-into). The order added is the order written.
        void addEntry(struct CajetaArchiveEntry entry);

        // Serialize to `path`. Creates parent directories if needed.
        // Throws std::runtime_error on I/O failure.
        void writeTo(const std::string& path);

        // Read an archive from `path`. Throws std::runtime_error on
        // missing file / bad magic / format-version mismatch / truncated
        // bytes. The returned archive carries the parsed manifest's
        // name / version / kind, plus every entry as a CajetaArchiveEntry.
        static CajetaArchive readFrom(const std::string& path);

        // Accessors used by readers (uber bundler, `cja` CLI tool, tests).
        const std::string& getName()    const { return name; }
        const std::string& getVersion() const { return version; }
        Kind               getKind()    const { return kind; }
        const std::vector<CajetaArchiveEntry>& getEntries() const { return entries; }

        // When this archive was loaded from disk (via readFrom), record
        // any dependency origins seen in the source archive's manifest.
        // emitArchive(uber) uses this to retag classpath-loaded entries
        // with their original source-archive name.
        void setSourceArchiveName(std::string s) { sourceArchiveName = std::move(s); }
        const std::string& getSourceArchiveName() const { return sourceArchiveName; }

    private:
        std::string name;
        std::string version;
        Kind        kind;
        std::vector<CajetaArchiveEntry> entries;
        std::string sourceArchiveName;   // set by readFrom; otherwise empty
        Compression compression = Compression::Zstd;

        // Build the manifest JSON. Format spelled out in Compilation.md
        // § Manifest extensions — name, version, kind, entry_count,
        // and (for uber) the origins map.
        std::string buildManifest() const;
    };

    struct CajetaArchiveEntry {
        std::string                    name;          // path-like with '/' separator
        uint8_t                        originTag = 0; // CajetaArchive::Origin cast to byte
        CajetaArchive::EntryKind       kindTag   = CajetaArchive::EntryKind::ClassBitcode;
        std::vector<uint8_t>           data;
    };

} // namespace cajeta
