// `.cja` (Cajeta ARchive) writer — v1 minimum-viable implementation
// of the container format spelled out in
// docs/specification/buildtool/Compilation.md § Archive format
// § Minimum-viable v1, which is authoritative and current.
//
// Layout produced by writeTo():
//   [ 32-byte header ]
//     8  bytes  magic          "CAJETA01"
//     4  bytes  format_version uint32 LE (currently 1)
//     4  bytes  flags          uint32 LE (bit 0 manifest_compressed,
//                              bit 1 entries_compressed)
//     8  bytes  index_offset   uint64 LE (start of the trailing index)
//     8  bytes  index_length   uint64 LE (its length in bytes)
//   [ manifest_length ]      uint64 LE — the ON-DISK size
//   [ manifest_bytes ]       UTF-8 JSON, compressed per the flags
//   [ entry_1 ]              uint32 name_length, name bytes,
//                            uint8 origin, uint8 kind, 2 reserved,
//                            then uint64 payload_length + payload
//   [ entry_2 ]
//   ...
//   [ trailing index ]       uint32 entry_count, then per entry
//                            (uint32 name_length, name bytes,
//                             uint64 entry_offset,
//                             uint64 entry_on_disk_size)
//
// Compression is ON BY DEFAULT — Compression::Zstd at level 3, over
// both the manifest and every entry; setCompression(None) opts out,
// which is what the tests probing raw header bytes do. A compressed
// section is framed `uint64 uncompressed_length || zstd_bytes`, and
// the length preceding it is the on-disk size, so a reader takes the
// allocation size from the inner prefix and the skip distance from
// the outer one.
//
// The trailing index is written by EVERY archive; index_offset and
// index_length are placeholders during the write and are patched into
// the header once the index has been appended and its offset is
// known. Readers treat `index_offset != 0` as "usable for random
// access" and may otherwise scan entries sequentially; findEntry()
// uses it for O(1) lookup by name.
//
// The index carries names, offsets and sizes ONLY. An entry's origin
// and kind tags live in the entry body, so anything classifying
// entries from the index alone — a registry enumerating an archive's
// classes without downloading it, say — is reading the entry name and
// nothing else.
//
// Used by Compiler::emitForModule when the user passes
// --emit=cja (project-only, library form) or --emit=uber
// (project + stdlib + transitively-referenced classpath deps,
// runnable form). Uber form populates the manifest with a `deps`
// array and nests each dep's entries under deps/<name>-<version>/;
// the container shape is otherwise identical.
//
// v1 omits the resources block and the runtime-bitcode block. Adding
// them later only flips header flag bits — the format-version field
// stays at 1 because additions land via flags + manifest fields, not
// version bumps.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta {

    class CajetaArchive {
    public:
        // Cja (project-only library) vs uber (project + stdlib +
        // transitively-referenced classpath deps) distinction.
        // Stored in the manifest's "kind" field; the on-disk
        // container is otherwise identical.
        enum class Kind {
            Cja,
            Uber,
        };

        // Entry kind tags written as a single byte on each entry.
        enum class EntryKind : uint8_t {
            ClassBitcode  = 0,   // LLVM bitcode for one cajeta class
            Resource      = 1,   // raw bytes (templates, configs, etc.)
            RuntimeBitcode = 2,  // C runtime bitcode (for --emit=exe inputs)
            ClassSource   = 3,   // Original .cajeta source bytes — used by
                                 // classpath ingestion to re-parse a dep's
                                 // classes into the consumer's compile.
            NativeArtifact = 4,  // per-platform native lib / header / metadata
                                 // under the `native/` tree (native-deps).
            KernelManifest = 5,  // xpu-tile-manifest §12.4: one tile-manifest-v1
                                 // JSON per (kernel, target) under
                                 // `xpu/manifests/<kernel>.<target>.manifest.json`,
                                 // beside the class bitcode that carries the
                                 // device code. Read-only, never consumed by
                                 // the linker; a tool reads it without loading
                                 // bitcode.
        };

        // Origin tags written as a single byte on each entry. Cja
        // archives carry only Origin::User (stdlib + deps stripped);
        // uber archives mix Origin::User (project code) with
        // Origin::Stdlib (parsed stdlib bundle) and Origin::Dependency
        // (entries nested under deps/<name>-<version>/).
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

        // zstd compression level (1-22). 1-3 are speed-optimized; 19-22
        // are ratio-optimized. The default of 3 is the .cja
        // write-few-read-many sweet spot. `cajeta archive repack
        // --zstd=<n>` is the user-facing surface; the compiler's
        // --emit=cja / --emit=uber paths keep the default.
        void setCompressionLevel(int level) { compressionLevel = level; }
        int  getCompressionLevel() const { return compressionLevel; }

        // Add one entry. Takes ownership of the entry's data vector
        // (move-into). The order added is the order written.
        void addEntry(struct CajetaArchiveEntry entry);

        // --- Native artifacts (native-deps subsystem, spec §3) ------------
        // A .cja carries native C/C++ artifacts under a `native/` tree:
        //   native/<os>-<arch>/<file>     per-platform linker inputs (.a/.so/…)
        //   native/include/<file>         shared headers / glue
        //   native/native-libraries.json  embedded native-library metadata
        // All tagged EntryKind::NativeArtifact.
        void addNativeArtifact(const std::string& platform,
                               const std::string& filename,
                               std::vector<uint8_t> data);
        void addNativeHeader(const std::string& filename,
                             std::vector<uint8_t> data);
        void setNativeLibrariesMeta(std::vector<uint8_t> json);

        // Platform triples with at least one native artifact.
        std::set<std::string> nativePlatforms() const;
        // Native linker artifacts for `platform` (empty if none — the named
        // "unsupported platform" error is the resolver's, not the reader's).
        std::vector<const CajetaArchiveEntry*>
            nativeArtifactsFor(const std::string& platform) const;
        // Shared native headers (native/include/…).
        std::vector<const CajetaArchiveEntry*> nativeHeaders() const;
        // Embedded native-library metadata entry, or null when slim.
        const CajetaArchiveEntry* nativeLibrariesMeta() const;

        // Serialize to `path`. Creates parent directories if needed.
        // Throws std::runtime_error on I/O failure.
        void writeTo(const std::string& path);

        // Serialize to an arbitrary output stream. Used by the
        // `cajeta archive` write subcommands (repack / strip / merge)
        // when the user passes `-` to mean stdout. The archive bytes
        // are buffered in memory first and then dumped in one shot,
        // because the writer needs to seek back to patch header
        // index_offset / index_length, and not all output streams
        // (stdout in particular) are seekable.
        void writeToStream(std::ostream& out);

        // Read an archive from `path`. Throws std::runtime_error on
        // missing file / bad magic / format-version mismatch / truncated
        // bytes. The returned archive carries the parsed manifest's
        // name / version / kind, plus every entry as a CajetaArchiveEntry.
        static CajetaArchive readFrom(const std::string& path);

        // Same as readFrom, but parses from an in-memory byte buffer.
        // Used by the `cajeta archive` read subcommands when the user
        // passes `-` to mean stdin (the CLI reads stdin into a vector
        // and hands it here). `sourceName` is what appears in error
        // diagnostics — typically `<stdin>` or a path string.
        static CajetaArchive readFromBytes(const std::vector<uint8_t>& bytes,
                                            const std::string& sourceName);

        // O(1) lookup by entry name. Returns a pointer to the entry, or
        // null if the name isn't present. The lookup table is built
        // eagerly (either from the on-disk trailing index when present,
        // or from a linear walk of the entries vector at the end of
        // readFrom), so this stays constant-time regardless of which
        // path produced the archive.
        const CajetaArchiveEntry* findEntry(const std::string& name) const;

        // Accessors used by readers (uber bundler, `cajeta archive`
        // subcommands, tests).
        const std::string& getName()    const { return name; }
        const std::string& getVersion() const { return version; }
        Kind               getKind()    const { return kind; }
        const std::vector<CajetaArchiveEntry>& getEntries() const { return entries; }

        // Raw on-disk manifest JSON as last loaded via readFrom. Empty
        // when this CajetaArchive was constructed in-memory (writer
        // path). The `cajeta archive info --json` subcommand prints
        // this verbatim so script consumers see the full field set,
        // not just the cherry-picked accessors above.
        const std::string& getRawManifest() const { return rawManifest; }
        void setRawManifest(std::string s) { rawManifest = std::move(s); }

        // When this archive was loaded from disk (via readFrom), record
        // any dependency origins seen in the source archive's manifest.
        // emitArchive(uber) uses this to retag classpath-loaded entries
        // with their original source-archive name.
        void setSourceArchiveName(std::string s) { sourceArchiveName = std::move(s); }
        const std::string& getSourceArchiveName() const { return sourceArchiveName; }

        // Per-dependency summary written into the manifest's "deps"
        // array. emitArchive(uber) builds one of these per classpath
        // archive that survives reachability pruning. Cja archives
        // never carry deps.
        struct DepSummary {
            std::string name;
            std::string version;
            uint32_t    includedEntryCount = 0;
        };
        void setDeps(std::vector<DepSummary> d) { deps = std::move(d); }
        const std::vector<DepSummary>& getDeps() const { return deps; }

    private:
        std::string name;
        std::string version;
        Kind        kind;
        std::vector<CajetaArchiveEntry> entries;
        std::vector<DepSummary>         deps;
        std::string sourceArchiveName;   // set by readFrom; otherwise empty
        std::string rawManifest;         // set by readFrom; otherwise empty
        Compression compression = Compression::Zstd;
        int         compressionLevel = 3;
        // Lazy lookup table — built on first findEntry call from the
        // entries vector. The trailing on-disk index that writeTo
        // produces is for FUTURE random-access readers; today's
        // findEntry just builds the in-memory map from the entries
        // vector itself, which is identical information.
        mutable std::unordered_map<std::string, std::size_t> nameIndex;

        // Build the manifest JSON. Format spelled out in Compilation.md
        // § Output formats § Manifest — name, version, kind,
        // entry_count, and (for uber) the deps array.
        std::string buildManifest() const;
    };

    struct CajetaArchiveEntry {
        std::string                    name;          // path-like with '/' separator
        uint8_t                        originTag = 0; // CajetaArchive::Origin cast to byte
        CajetaArchive::EntryKind       kindTag   = CajetaArchive::EntryKind::ClassBitcode;
        std::vector<uint8_t>           data;
    };

} // namespace cajeta
