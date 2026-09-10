// `.cja` (Cajeta ARchive) reader/writer; all integers little-endian. The container
// format — header, manifest, entry encoding, trailing index, compressed framing —
// is specified in docs/specification/buildtool/Compilation.md § Archive format.

#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace cajeta {

    class CajetaArchive {
    public:
        // Cja (project only) vs uber (+ stdlib + reachable deps): the manifest's
        // "kind" field only — the container is identical either way.
        enum class Kind {
            Cja,
            Uber,
        };

        // Entry kind tags written as a single byte on each entry.
        enum class EntryKind : uint8_t {
            ClassBitcode  = 0,   // LLVM bitcode for one cajeta class
            Resource      = 1,   // raw bytes (templates, configs, etc.)
            RuntimeBitcode = 2,  // C runtime bitcode (for --emit=exe inputs)
            ClassSource   = 3,   // .cajeta source, re-parsed by classpath ingest
            NativeArtifact = 4,  // native lib / header / metadata under native/
            KernelManifest = 5,  // tile-manifest-v1 JSON under xpu/manifests/
        };

        // Origin tags, one byte per entry: cja archives carry only User, uber
        // archives mix in Stdlib and Dependency (nested under deps/<n>-<ver>/).
        enum class Origin : uint8_t {
            User    = 0,
            Stdlib  = 1,
            Dependency = 2,
        };

        // Compression for both the manifest and entry payloads. The reader
        // auto-detects from the header flag bits, whatever the writer chose.
        enum class Compression {
            None,
            Zstd,
        };

        CajetaArchive(std::string name, std::string version, Kind kind);

        // Writer algorithm; Zstd by default, None to keep byte offsets raw.
        void setCompression(Compression c) { compression = c; }
        Compression getCompression() const { return compression; }

        // zstd level 1-22 (1-3 speed, 19-22 ratio); --emit=cja/uber keep 3.
        void setCompressionLevel(int level) { compressionLevel = level; }
        int  getCompressionLevel() const { return compressionLevel; }

        // Add one entry, taking ownership of its data; added order = write order.
        void addEntry(struct CajetaArchiveEntry entry);

        // Native C/C++ artifacts, all tagged EntryKind::NativeArtifact, under
        // native/<os>-<arch>/<file> (linker inputs), native/include/<file>
        // (headers) and native/native-libraries.json (metadata).
        void addNativeArtifact(const std::string& platform,
                               const std::string& filename,
                               std::vector<uint8_t> data);
        void addNativeHeader(const std::string& filename,
                             std::vector<uint8_t> data);
        void setNativeLibrariesMeta(std::vector<uint8_t> json);

        // Platform triples with at least one native artifact.
        std::set<std::string> nativePlatforms() const;
        // Native linker artifacts for `platform`; empty when none, since the
        // "unsupported platform" error is the resolver's, not the reader's.
        std::vector<const CajetaArchiveEntry*>
            nativeArtifactsFor(const std::string& platform) const;
        // Shared native headers (native/include/…).
        std::vector<const CajetaArchiveEntry*> nativeHeaders() const;
        // Embedded native-library metadata entry, or null when slim.
        const CajetaArchiveEntry* nativeLibrariesMeta() const;

        // Serialize to `path`, creating parent dirs; throws on I/O failure.
        void writeTo(const std::string& path);

        // Serialize to any output stream (`cajeta archive` writing to stdout).
        // Buffers the whole archive first: patching index_offset/index_length
        // needs a seek back, and stdout is not seekable.
        void writeToStream(std::ostream& out);

        // Read an archive from `path`, manifest fields and all entries; throws
        // on a missing, mis-magicked, wrong-version or truncated file.
        static CajetaArchive readFrom(const std::string& path);

        // readFrom over an in-memory buffer (the CLI's `-` = stdin path).
        // `sourceName` is the name error diagnostics report.
        static CajetaArchive readFromBytes(const std::vector<uint8_t>& bytes,
                                            const std::string& sourceName);

        // O(1) lookup by entry name, null when absent.
        const CajetaArchiveEntry* findEntry(const std::string& name) const;

        const std::string& getName()    const { return name; }
        const std::string& getVersion() const { return version; }
        Kind               getKind()    const { return kind; }
        const std::vector<CajetaArchiveEntry>& getEntries() const { return entries; }

        // Raw manifest JSON as loaded by readFrom, empty on the writer path;
        // `cajeta archive info --json` prints it verbatim.
        const std::string& getRawManifest() const { return rawManifest; }
        void setRawManifest(std::string s) { rawManifest = std::move(s); }

        // The source archive a readFrom'd entry came from; emitArchive(uber)
        // retags classpath-loaded entries with it.
        void setSourceArchiveName(std::string s) { sourceArchiveName = std::move(s); }
        const std::string& getSourceArchiveName() const { return sourceArchiveName; }

        // One manifest "deps" entry per classpath archive that survives
        // reachability pruning. Cja archives never carry deps.
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
        mutable std::unordered_map<std::string, std::size_t> nameIndex;

        // Build the manifest JSON: name, version, kind, entry_count and, for
        // uber archives, the deps array.
        std::string buildManifest() const;
    };

    struct CajetaArchiveEntry {
        std::string                    name;          // path-like with '/' separator
        uint8_t                        originTag = 0; // CajetaArchive::Origin cast to byte
        CajetaArchive::EntryKind       kindTag   = CajetaArchive::EntryKind::ClassBitcode;
        std::vector<uint8_t>           data;
    };

} // namespace cajeta
