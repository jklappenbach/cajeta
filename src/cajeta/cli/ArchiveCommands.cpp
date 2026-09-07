#include "ArchiveCommands.h"

#include "../compile/CajetaArchive.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cajeta {

namespace {

// ---------------------------------------------------------------- exit codes
// Mirror ArchiveManagement.md §4. Keep the table in lockstep with that doc.
enum : int {
    EXIT_OK              = 0,
    EXIT_USAGE           = 1,
    EXIT_NOT_FOUND       = 2,
    EXIT_BAD_MAGIC       = 3,
    EXIT_UNSUPPORTED_FMT = 4,
    EXIT_TRUNCATED       = 5,
    EXIT_CORRUPT         = 6,
    EXIT_CHECKSUM        = 7,
    EXIT_IO              = 8,
    EXIT_DIFF            = 9,
    EXIT_COLLISION       = 10,
    EXIT_SIG_INVALID     = 11,
};

// ---------------------------------------------------------------- common flags
struct CommonFlags {
    bool json  = false;
    bool quiet = false;
};

// Strip a `--json` / `--quiet` / `-q` flag out of `args` in-place,
// updating `flags`. Run before subcommand-specific flag parsing so
// each handler only sees what's left.
void consumeCommonFlags(std::vector<std::string>& args, CommonFlags& flags) {
    auto it = args.begin();
    while (it != args.end()) {
        if (*it == "--json")             { flags.json = true;  it = args.erase(it); }
        else if (*it == "--quiet" || *it == "-q") { flags.quiet = true; it = args.erase(it); }
        else { ++it; }
    }
}

// ---------------------------------------------------------------- help text
void printArchiveUsage() {
    std::cerr <<
        "Usage: cajeta archive <subcommand> [args...]\n"
        "\n"
        "Read subcommands:\n"
        "  list <archive> [paths...]           List entries (name, kind, origin, size).\n"
        "  cat <archive> <entry-path>          Dump one entry's bytes to stdout.\n"
        "  extract <archive> [-C dir] [paths]  Explode entries to a directory.\n"
        "  info <archive>                      Print manifest fields.\n"
        "  deps <archive>                      Print the deps array (uber archives).\n"
        "  verify <archive> [--strict]         Structural integrity check.\n"
        "  diff <a.cja> <b.cja>                Entry-by-entry diff.\n"
        "\n"
        "Write subcommands:\n"
        "  repack <in> <out> [--zstd=<n>]      Re-emit with different compression.\n"
        "  strip <in> <out> [--exclude/--include=<glob>]\n"
        "                                      Filter entries by name glob.\n"
        "  merge <out> <a> <b> [...]           Combine multiple archives.\n"
        "\n"
        "Signing:\n"
        "  sign <archive> --key <pem> [--out <sig>]\n"
        "                                      ed25519 detached signature.\n"
        "  verify-sig <archive> --pubkey <pem> [--sig <sig>]\n"
        "                                      Verify a detached signature.\n"
        "\n"
        "Pipe conventions:\n"
        "  <archive> accepts `-` to read from stdin (cat / list / info / deps / verify).\n"
        "  <out> accepts `-` to write to stdout (repack / strip / merge).\n"
        "\n"
        "Global flags:\n"
        "  --json                              Machine-readable JSON output.\n"
        "  --quiet, -q                         Suppress non-error output.\n"
        "\n"
        "Spec: docs/ArchiveManagement.md\n";
}

// ---------------------------------------------------------------- stdin / stdout helpers
// Read every byte from stdin until EOF. Used when an archive path is "-".
std::vector<uint8_t> readStdinBytes() {
    std::vector<uint8_t> bytes;
    constexpr size_t CHUNK = 65536;
    bytes.reserve(CHUNK);
    char buf[CHUNK];
    while (true) {
        std::cin.read(buf, CHUNK);
        std::streamsize got = std::cin.gcount();
        if (got <= 0) break;
        bytes.insert(bytes.end(), (uint8_t*) buf, (uint8_t*) buf + got);
        if (std::cin.eof()) break;
    }
    return bytes;
}

// Read a file's bytes (or stdin if path == "-"). Throws on I/O failure.
std::vector<uint8_t> readPathOrStdin(const std::string& path) {
    if (path == "-") return readStdinBytes();
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open: " + path);
    }
    return std::vector<uint8_t>(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());
}

// ---------------------------------------------------------------- glob matching
// Recursive shell-style match supporting `*` (single segment), `**` (any
// segments), `?` (one char). Anchored at both ends.
bool globMatch(const char* pat, const char* str) {
    while (*pat) {
        if (pat[0] == '*' && pat[1] == '*') {
            // ** matches any number of chars including '/'. Try every
            // suffix of `str` after consuming the `**`.
            const char* afterStars = pat + 2;
            if (*afterStars == '/') ++afterStars;
            // Empty remainder matches everything left.
            if (*afterStars == 0) return true;
            for (const char* s = str; ; ++s) {
                if (globMatch(afterStars, s)) return true;
                if (*s == 0) return false;
            }
        }
        if (*pat == '*') {
            // * matches any chars except '/'.
            const char* after = pat + 1;
            for (const char* s = str; ; ++s) {
                if (globMatch(after, s)) return true;
                if (*s == 0 || *s == '/') return false;
            }
        }
        if (*pat == '?') {
            if (*str == 0 || *str == '/') return false;
            ++pat; ++str; continue;
        }
        if (*pat != *str) return false;
        ++pat; ++str;
    }
    return *str == 0;
}

bool entryMatchesAnyPath(const std::string& entryName,
                         const std::vector<std::string>& patterns) {
    if (patterns.empty()) return true;
    for (const auto& p : patterns) {
        // Exact prefix match (foo/bar/ matches foo/bar/baz.bc) and
        // glob match both supported. A naked path without * is treated
        // as a prefix; a path with glob metachars goes through the
        // glob matcher unchanged.
        if (p.find('*') != std::string::npos || p.find('?') != std::string::npos) {
            if (globMatch(p.c_str(), entryName.c_str())) return true;
        } else {
            // Prefix or exact match.
            if (entryName == p) return true;
            if (entryName.size() > p.size()
                && entryName.compare(0, p.size(), p) == 0
                && (p.back() == '/' || entryName[p.size()] == '/')) {
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------------- formatting helpers
const char* originName(uint8_t tag) {
    switch ((CajetaArchive::Origin) tag) {
        case CajetaArchive::Origin::User:       return "user";
        case CajetaArchive::Origin::Stdlib:     return "stdlib";
        case CajetaArchive::Origin::Dependency: return "dep";
    }
    return "?";
}

const char* kindName(CajetaArchive::EntryKind k) {
    switch (k) {
        case CajetaArchive::EntryKind::ClassBitcode:   return "class_bitcode";
        case CajetaArchive::EntryKind::Resource:       return "resource";
        case CajetaArchive::EntryKind::RuntimeBitcode: return "runtime_bitcode";
        case CajetaArchive::EntryKind::ClassSource:    return "class_source";
        case CajetaArchive::EntryKind::KernelManifest: return "kernel_manifest";
    }
    return "?";
}

const char* archiveKindName(CajetaArchive::Kind k) {
    return k == CajetaArchive::Kind::Uber ? "uber" : "cja";
}

// xxh3-64 as a 16-hex-char string. Stable representation across diffs.
std::string xxh3Hex(const std::vector<uint8_t>& bytes) {
    XXH64_hash_t h = XXH3_64bits(bytes.data(), bytes.size());
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long) h);
    return std::string(buf);
}

// Minimal JSON string escape — handles the subset cajeta entry names
// and manifest values actually use ('"' and '\\' and control bytes).
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char tmp[8];
                    std::snprintf(tmp, sizeof(tmp), "\\u%04x", c);
                    out += tmp;
                } else {
                    out += (char) c;
                }
        }
    }
    return out;
}

// ---------------------------------------------------------------- archive load
// Wraps CajetaArchive::readFrom and maps the underlying exception
// text to a stable exit code. The CajetaArchive reader composes its
// messages from a small vocabulary ("bad magic", "unsupported format
// version", "truncated", ...), so a substring match here is robust
// enough for the §4 exit-code mapping.
struct LoadResult {
    bool ok = false;
    int exitCode = EXIT_OK;
    CajetaArchive archive = CajetaArchive("", "", CajetaArchive::Kind::Cja);
};

LoadResult loadArchiveOrReport(const std::string& path) {
    LoadResult r;
    try {
        if (path == "-") {
            auto bytes = readStdinBytes();
            r.archive = CajetaArchive::readFromBytes(bytes, "<stdin>");
        } else {
            r.archive = CajetaArchive::readFrom(path);
        }
        r.ok = true;
    } catch (const std::exception& e) {
        std::string msg = e.what();
        std::cerr << "cajeta archive: " << msg << "\n";
        if (msg.find("bad magic") != std::string::npos)              r.exitCode = EXIT_BAD_MAGIC;
        else if (msg.find("unsupported format version") != std::string::npos) r.exitCode = EXIT_UNSUPPORTED_FMT;
        else if (msg.find("truncated") != std::string::npos
              || msg.find("too short") != std::string::npos)         r.exitCode = EXIT_TRUNCATED;
        else if (msg.find("cannot open") != std::string::npos) {
            // Distinguish missing file from permission/IO failure by
            // probing the path. The archive reader's "cannot open"
            // message covers both ENOENT and EACCES; the §4 exit
            // codes split them.
            if (errno == ENOENT
                || !std::filesystem::exists(path))                  r.exitCode = EXIT_NOT_FOUND;
            else                                                     r.exitCode = EXIT_IO;
        }
        else if (msg.find("could not read") != std::string::npos)   r.exitCode = EXIT_IO;
        else                                                          r.exitCode = EXIT_CORRUPT;
    }
    return r;
}

// ---------------------------------------------------------------- manifest field scan
// Pull a top-level string field's value out of the raw manifest JSON.
// The manifest writer produces compact JSON with no whitespace and no
// escapes inside the values cajeta writes today, so a substring scan
// is sufficient. Returns the empty string when the key is absent.
std::string scanString(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return "";
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// Pull a top-level numeric field's value. Returns 0 when absent.
uint64_t scanNumber(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0;
    pos += needle.size();
    // Skip optional whitespace (manifest writer doesn't emit any, but
    // be defensive against future formatting).
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) ++pos;
    uint64_t v = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        v = v * 10 + (uint64_t)(json[pos] - '0');
        ++pos;
    }
    return v;
}

// ---------------------------------------------------------------- list
int cmdList(const std::vector<std::string>& args, const CommonFlags& f) {
    if (args.empty()) {
        std::cerr << "cajeta archive list: missing <archive>\n";
        return EXIT_USAGE;
    }
    auto load = loadArchiveOrReport(args[0]);
    if (!load.ok) return load.exitCode;

    std::vector<std::string> pathFilters(args.begin() + 1, args.end());

    // Snapshot + sort by name for stable output.
    std::vector<const CajetaArchiveEntry*> view;
    view.reserve(load.archive.getEntries().size());
    for (const auto& e : load.archive.getEntries()) {
        if (entryMatchesAnyPath(e.name, pathFilters)) {
            view.push_back(&e);
        }
    }
    std::sort(view.begin(), view.end(),
        [](const CajetaArchiveEntry* a, const CajetaArchiveEntry* b) {
            return a->name < b->name;
        });

    if (f.json) {
        std::cout << "{\"archive\":\"" << jsonEscape(args[0])
                  << "\",\"entry_count\":" << view.size()
                  << ",\"entries\":[";
        bool first = true;
        for (const auto* e : view) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"name\":\""    << jsonEscape(e->name) << "\""
                      << ",\"kind\":\""    << kindName(e->kindTag) << "\""
                      << ",\"origin\":\""  << originName(e->originTag) << "\""
                      << ",\"size\":"      << e->data.size()
                      << "}";
        }
        std::cout << "]}\n";
        return EXIT_OK;
    }

    if (!f.quiet) {
        std::cout << "KIND             ORIGIN  SIZE       NAME\n";
    }
    for (const auto* e : view) {
        std::cout << std::left << std::setw(16) << kindName(e->kindTag) << " "
                  << std::left << std::setw(6)  << originName(e->originTag) << "  "
                  << std::right << std::setw(9) << e->data.size() << "  "
                  << e->name << "\n";
    }
    return EXIT_OK;
}

// ---------------------------------------------------------------- cat
int cmdCat(const std::vector<std::string>& args, const CommonFlags& /*f*/) {
    if (args.size() < 2) {
        std::cerr << "cajeta archive cat: usage: cat <archive> <entry-path>\n";
        return EXIT_USAGE;
    }
    auto load = loadArchiveOrReport(args[0]);
    if (!load.ok) return load.exitCode;

    const auto* e = load.archive.findEntry(args[1]);
    if (!e) {
        std::cerr << "cajeta archive: entry not found: " << args[1] << "\n";
        return EXIT_NOT_FOUND;
    }

    // Raw binary write to stdout. Bypass std::cout's formatting (some
    // platforms munge \r\n on text-mode handles); write directly to
    // the underlying file descriptor via fwrite on stdout.
    std::fwrite(e->data.data(), 1, e->data.size(), stdout);
    std::fflush(stdout);
    return EXIT_OK;
}

// ---------------------------------------------------------------- extract
int cmdExtract(const std::vector<std::string>& args, const CommonFlags& f) {
    // Parse flags + positional args in one pass: -C <dir>, --overwrite,
    // --flatten, --strip=<n>, then positionals = <archive> [paths...].
    std::string destDir = ".";
    bool overwrite = false;
    bool flatten   = false;
    int  stripN    = 0;
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "-C") {
            if (i + 1 >= args.size()) {
                std::cerr << "cajeta archive extract: -C requires a directory argument\n";
                return EXIT_USAGE;
            }
            destDir = args[++i];
        } else if (a == "--overwrite") {
            overwrite = true;
        } else if (a == "--flatten") {
            flatten = true;
        } else if (a.rfind("--strip=", 0) == 0) {
            stripN = std::atoi(a.c_str() + 8);
            if (stripN < 0) stripN = 0;
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "cajeta archive extract: unknown flag: " << a << "\n";
            return EXIT_USAGE;
        } else {
            positional.push_back(a);
        }
    }
    if (flatten && stripN > 0) {
        std::cerr << "cajeta archive extract: --flatten and --strip are mutually exclusive\n";
        return EXIT_USAGE;
    }
    if (positional.empty()) {
        std::cerr << "cajeta archive extract: missing <archive>\n";
        return EXIT_USAGE;
    }

    auto load = loadArchiveOrReport(positional[0]);
    if (!load.ok) return load.exitCode;

    std::vector<std::string> filters(positional.begin() + 1, positional.end());

    std::filesystem::path base(destDir);
    std::error_code ec;
    std::filesystem::create_directories(base, ec);
    if (ec) {
        std::cerr << "cajeta archive extract: cannot create " << destDir
                  << ": " << ec.message() << "\n";
        return EXIT_IO;
    }

    for (const auto& e : load.archive.getEntries()) {
        if (!entryMatchesAnyPath(e.name, filters)) continue;

        // Compute the on-disk relative path with flatten/strip applied.
        std::string outRel;
        if (flatten) {
            auto slash = e.name.find_last_of('/');
            outRel = (slash == std::string::npos) ? e.name
                                                  : e.name.substr(slash + 1);
        } else if (stripN > 0) {
            int dropped = 0;
            size_t pos = 0;
            while (dropped < stripN) {
                auto slash = e.name.find('/', pos);
                if (slash == std::string::npos) { pos = e.name.size(); break; }
                pos = slash + 1;
                ++dropped;
            }
            outRel = (pos >= e.name.size()) ? std::string() : e.name.substr(pos);
            if (outRel.empty()) continue;   // entry name entirely stripped
        } else {
            outRel = e.name;
        }

        std::filesystem::path outPath = base / outRel;
        // Defense against archive entries with .. components — refuse
        // to write outside the destination dir.
        auto canonicalBase = std::filesystem::weakly_canonical(base);
        auto canonicalOut  = std::filesystem::weakly_canonical(outPath);
        auto rel = std::filesystem::relative(canonicalOut, canonicalBase);
        if (!rel.empty() && rel.native()[0] == '.' && rel.native().size() > 1
                          && rel.native()[1] == '.') {
            std::cerr << "cajeta archive extract: refusing to write outside dest: "
                      << e.name << "\n";
            return EXIT_IO;
        }

        if (std::filesystem::exists(outPath) && !overwrite) {
            if (!f.quiet) {
                std::cerr << "skip (exists): " << outPath.string() << "\n";
            }
            continue;
        }

        std::filesystem::create_directories(outPath.parent_path(), ec);
        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "cajeta archive extract: cannot write " << outPath.string() << "\n";
            return EXIT_IO;
        }
        out.write((const char*) e.data.data(), (std::streamsize) e.data.size());
        if (!out.good()) {
            std::cerr << "cajeta archive extract: short write " << outPath.string() << "\n";
            return EXIT_IO;
        }
        if (!f.quiet) {
            std::cout << outPath.string() << "\n";
        }
    }
    return EXIT_OK;
}

// ---------------------------------------------------------------- info
int cmdInfo(const std::vector<std::string>& args, const CommonFlags& f) {
    if (args.empty()) {
        std::cerr << "cajeta archive info: missing <archive>\n";
        return EXIT_USAGE;
    }
    auto load = loadArchiveOrReport(args[0]);
    if (!load.ok) return load.exitCode;

    if (f.json) {
        // Dump the raw manifest verbatim. Per §5, --json is the stable
        // format; the manifest schema additions are backward-compatible.
        std::cout << load.archive.getRawManifest() << "\n";
        return EXIT_OK;
    }

    const auto& manifest = load.archive.getRawManifest();
    uint64_t totalSize = 0;
    for (const auto& e : load.archive.getEntries()) totalSize += e.data.size();

    auto emit = [&](const char* label, const std::string& v) {
        if (!v.empty()) std::cout << std::left << std::setw(22) << label << v << "\n";
    };

    emit("name:",                load.archive.getName());
    emit("version:",             load.archive.getVersion());
    emit("kind:",                archiveKindName(load.archive.getKind()));
    emit("cajeta_lang_version:", scanString(manifest, "cajeta_lang_version"));
    emit("build_flavor:",        scanString(manifest, "build_flavor"));
    emit("build_timestamp:",     scanString(manifest, "build_timestamp"));
    emit("target_triple:",       scanString(manifest, "target_triple"));
    std::cout << std::left << std::setw(22) << "entry_count:"
              << load.archive.getEntries().size() << "\n";
    std::cout << std::left << std::setw(22) << "deps_count:"
              << load.archive.getDeps().size() << "\n";
    std::cout << std::left << std::setw(22) << "total_uncompressed:"
              << totalSize << " bytes\n";
    return EXIT_OK;
}

// ---------------------------------------------------------------- deps
int cmdDeps(const std::vector<std::string>& args, const CommonFlags& f) {
    if (args.empty()) {
        std::cerr << "cajeta archive deps: missing <archive>\n";
        return EXIT_USAGE;
    }
    auto load = loadArchiveOrReport(args[0]);
    if (!load.ok) return load.exitCode;

    const auto& deps = load.archive.getDeps();
    if (f.json) {
        std::cout << "[";
        bool first = true;
        for (const auto& d : deps) {
            if (!first) std::cout << ",";
            first = false;
            std::cout << "{\"name\":\""    << jsonEscape(d.name)    << "\""
                      << ",\"version\":\"" << jsonEscape(d.version) << "\""
                      << ",\"included_entry_count\":" << d.includedEntryCount
                      << "}";
        }
        std::cout << "]\n";
        return EXIT_OK;
    }
    for (const auto& d : deps) {
        std::cout << std::left << std::setw(28) << d.name
                  << std::left << std::setw(12) << d.version
                  << d.includedEntryCount << " entries\n";
    }
    return EXIT_OK;
}

// ---------------------------------------------------------------- verify
int cmdVerify(const std::vector<std::string>& args, const CommonFlags& f) {
    bool strict = false;
    std::vector<std::string> positional;
    for (const auto& a : args) {
        if (a == "--strict") strict = true;
        else if (a.rfind("--", 0) == 0) {
            std::cerr << "cajeta archive verify: unknown flag: " << a << "\n";
            return EXIT_USAGE;
        } else positional.push_back(a);
    }
    if (positional.empty()) {
        std::cerr << "cajeta archive verify: missing <archive>\n";
        return EXIT_USAGE;
    }
    // The readFrom path already does the heavy lifting: it validates
    // the magic, format version, header bounds, decompresses the
    // manifest, parses it, walks every entry, decompresses each
    // entry's payload, and confirms no truncation. We re-run it here
    // and add the duplicate-name + manifest-required-field checks
    // that readFrom doesn't enforce.
    auto load = loadArchiveOrReport(positional[0]);
    if (!load.ok) return load.exitCode;

    std::unordered_set<std::string> seen;
    for (const auto& e : load.archive.getEntries()) {
        if (!seen.insert(e.name).second) {
            std::cerr << "cajeta archive verify: duplicate entry name: "
                      << e.name << "\n";
            return EXIT_CORRUPT;
        }
    }
    if (load.archive.getName().empty()) {
        std::cerr << "cajeta archive verify: manifest missing required field: name\n";
        return EXIT_CORRUPT;
    }
    if (load.archive.getVersion().empty()) {
        std::cerr << "cajeta archive verify: manifest missing required field: version\n";
        return EXIT_CORRUPT;
    }

    // --strict: enforce the "recommended-but-optional" manifest fields.
    // build_timestamp + cajeta_lang_version are the two the spec calls
    // out — both should be populated by every release-flavored build.
    // Their absence in --strict mode signals a non-canonical archive.
    if (strict) {
        const auto& m = load.archive.getRawManifest();
        if (scanString(m, "build_timestamp").empty()) {
            std::cerr << "cajeta archive verify --strict: missing build_timestamp\n";
            return EXIT_CORRUPT;
        }
        if (scanString(m, "cajeta_lang_version").empty()) {
            std::cerr << "cajeta archive verify --strict: missing cajeta_lang_version\n";
            return EXIT_CORRUPT;
        }
    }

    if (!f.quiet) {
        std::cout << "ok: " << positional[0] << " (" << load.archive.getEntries().size()
                  << " entries, kind=" << archiveKindName(load.archive.getKind())
                  << (strict ? ", strict" : "")
                  << ")\n";
    }
    return EXIT_OK;
}

// ---------------------------------------------------------------- diff
int cmdDiff(const std::vector<std::string>& args, const CommonFlags& f) {
    bool nameOnly      = false;
    bool includeStdlib = false;
    std::vector<std::string> positional;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--name-only")           nameOnly = true;
        else if (a == "--include-stdlib") includeStdlib = true;
        else if (a.rfind("--", 0) == 0) {
            std::cerr << "cajeta archive diff: unknown flag: " << a << "\n";
            return EXIT_USAGE;
        }
        else positional.push_back(a);
    }
    if (positional.size() < 2) {
        std::cerr << "cajeta archive diff: usage: diff <a.cja> <b.cja>\n";
        return EXIT_USAGE;
    }

    auto la = loadArchiveOrReport(positional[0]);
    if (!la.ok) return la.exitCode;
    auto lb = loadArchiveOrReport(positional[1]);
    if (!lb.ok) return lb.exitCode;

    // Index each side by name, skipping stdlib entries by default.
    auto buildIndex = [&](const CajetaArchive& arc) {
        std::unordered_map<std::string, const CajetaArchiveEntry*> idx;
        for (const auto& e : arc.getEntries()) {
            if (!includeStdlib && e.originTag == (uint8_t) CajetaArchive::Origin::Stdlib) continue;
            idx[e.name] = &e;
        }
        return idx;
    };
    auto idxA = buildIndex(la.archive);
    auto idxB = buildIndex(lb.archive);

    std::vector<std::string> added, removed;
    struct Changed {
        std::string name;
        size_t aSize, bSize;
        std::string aHash, bHash;
    };
    std::vector<Changed> changed;
    size_t identicalCount = 0;

    // All names present in either side.
    std::set<std::string> allNames;
    for (const auto& kv : idxA) allNames.insert(kv.first);
    for (const auto& kv : idxB) allNames.insert(kv.first);

    for (const auto& name : allNames) {
        const auto* eA = idxA.count(name) ? idxA[name] : nullptr;
        const auto* eB = idxB.count(name) ? idxB[name] : nullptr;
        if (eA && !eB)      removed.push_back(name);
        else if (!eA && eB) added.push_back(name);
        else if (eA && eB) {
            if (nameOnly) { ++identicalCount; continue; }
            if (eA->data.size() == eB->data.size()
                && std::memcmp(eA->data.data(), eB->data.data(), eA->data.size()) == 0) {
                ++identicalCount;
            } else {
                changed.push_back({name, eA->data.size(), eB->data.size(),
                                   xxh3Hex(eA->data), xxh3Hex(eB->data)});
            }
        }
    }

    if (f.json) {
        std::cout << "{\"added\":[";
        for (size_t i = 0; i < added.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "\"" << jsonEscape(added[i]) << "\"";
        }
        std::cout << "],\"removed\":[";
        for (size_t i = 0; i < removed.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "\"" << jsonEscape(removed[i]) << "\"";
        }
        std::cout << "],\"changed\":[";
        for (size_t i = 0; i < changed.size(); ++i) {
            if (i) std::cout << ",";
            std::cout << "{\"name\":\""   << jsonEscape(changed[i].name) << "\""
                      << ",\"a_size\":"   << changed[i].aSize
                      << ",\"b_size\":"   << changed[i].bSize
                      << ",\"a_xxh3\":\"" << changed[i].aHash << "\""
                      << ",\"b_xxh3\":\"" << changed[i].bHash << "\""
                      << "}";
        }
        std::cout << "],\"identical_count\":" << identicalCount << "}\n";
    } else if (!f.quiet) {
        for (const auto& n : added)   std::cout << "+ " << n << "  (added in " << positional[1] << ")\n";
        for (const auto& n : removed) std::cout << "- " << n << "  (removed from " << positional[1] << ")\n";
        for (const auto& c : changed) std::cout << "~ " << c.name
                                                << "  (" << c.aSize << " -> " << c.bSize << " bytes)\n";
    }

    bool anyDiff = !added.empty() || !removed.empty() || !changed.empty();
    return anyDiff ? EXIT_DIFF : EXIT_OK;
}

// ---------------------------------------------------------------- archive write helper
// Materialize an in-memory CajetaArchive to a path or to stdout when
// `outPath == "-"`. Used uniformly by repack / strip / merge so the
// `-` convention from ArchiveManagement.md §6 is honored everywhere.
int writeArchiveOrReport(CajetaArchive& arc, const std::string& outPath,
                          const char* subcommand) {
    try {
        if (outPath == "-") {
            arc.writeToStream(std::cout);
            std::cout.flush();
        } else {
            arc.writeTo(outPath);
        }
    } catch (const std::exception& e) {
        std::cerr << "cajeta archive " << subcommand << ": " << e.what() << "\n";
        return EXIT_IO;
    }
    return EXIT_OK;
}

// Build a fresh CajetaArchiveEntry from an existing one's fields. The
// writer-side `addEntry` takes by-move; this helper makes the "copy
// one entry from a loaded archive into a new archive" pattern a
// one-liner.
CajetaArchiveEntry cloneEntry(const CajetaArchiveEntry& src) {
    CajetaArchiveEntry e;
    e.name      = src.name;
    e.originTag = src.originTag;
    e.kindTag   = src.kindTag;
    e.data      = src.data;
    return e;
}

// ---------------------------------------------------------------- repack
int cmdRepack(const std::vector<std::string>& args, const CommonFlags& /*f*/) {
    int  zstdLevel        = 3;
    bool compressionNone  = false;
    std::vector<std::string> positional;
    for (const auto& a : args) {
        if (a.rfind("--zstd=", 0) == 0) {
            zstdLevel = std::atoi(a.c_str() + 7);
            if (zstdLevel < 1 || zstdLevel > 22) {
                std::cerr << "cajeta archive repack: --zstd level out of range "
                             "(1-22): " << zstdLevel << "\n";
                return EXIT_USAGE;
            }
        } else if (a == "--compression=none") {
            compressionNone = true;
        } else if (a == "--keep-index") {
            // No-op — the writer always emits the trailing index today.
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "cajeta archive repack: unknown flag: " << a << "\n";
            return EXIT_USAGE;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() < 2) {
        std::cerr << "cajeta archive repack: usage: repack <in> <out>\n";
        return EXIT_USAGE;
    }

    auto load = loadArchiveOrReport(positional[0]);
    if (!load.ok) return load.exitCode;

    CajetaArchive out(load.archive.getName(), load.archive.getVersion(),
                      load.archive.getKind());
    out.setCompression(compressionNone
        ? CajetaArchive::Compression::None
        : CajetaArchive::Compression::Zstd);
    out.setCompressionLevel(zstdLevel);
    for (const auto& e : load.archive.getEntries()) {
        out.addEntry(cloneEntry(e));
    }
    if (load.archive.getKind() == CajetaArchive::Kind::Uber) {
        out.setDeps(load.archive.getDeps());
    }
    return writeArchiveOrReport(out, positional[1], "repack");
}

// ---------------------------------------------------------------- strip
int cmdStrip(const std::vector<std::string>& args, const CommonFlags& /*f*/) {
    std::vector<std::string> includes;
    std::vector<std::string> excludes;
    std::vector<std::string> positional;
    for (const auto& a : args) {
        if (a.rfind("--exclude=", 0) == 0) {
            excludes.emplace_back(a.substr(10));
        } else if (a.rfind("--include=", 0) == 0) {
            includes.emplace_back(a.substr(10));
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "cajeta archive strip: unknown flag: " << a << "\n";
            return EXIT_USAGE;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.size() < 2) {
        std::cerr << "cajeta archive strip: usage: strip <in> <out>\n";
        return EXIT_USAGE;
    }

    auto load = loadArchiveOrReport(positional[0]);
    if (!load.ok) return load.exitCode;

    CajetaArchive out(load.archive.getName(), load.archive.getVersion(),
                      load.archive.getKind());
    // Compression settings track the input (zstd default; tests that
    // need raw bytes can repack afterwards with --compression=none).

    // Apply filters: kept iff (includes empty || matches an include)
    //                       AND not matched by any exclude.
    for (const auto& e : load.archive.getEntries()) {
        bool keep = includes.empty() || entryMatchesAnyPath(e.name, includes);
        if (keep && !excludes.empty()
                 && entryMatchesAnyPath(e.name, excludes)) {
            keep = false;
        }
        if (keep) out.addEntry(cloneEntry(e));
    }

    // Deps array fixup: walk the loaded deps; for each dep, count the
    // remaining kept entries under deps/<name>-<version>/. Prune deps
    // whose entry count drops to zero; update includedEntryCount for
    // the rest. The spec's "error if --exclude would drop entries the
    // manifest's metadata still references" clause is satisfied
    // implicitly — we keep the deps array in sync.
    if (load.archive.getKind() == CajetaArchive::Kind::Uber) {
        std::vector<CajetaArchive::DepSummary> newDeps;
        for (const auto& d : load.archive.getDeps()) {
            std::string prefix = "deps/" + d.name + "-" + d.version + "/";
            uint32_t remaining = 0;
            for (const auto& e : out.getEntries()) {
                if (e.name.size() > prefix.size()
                    && e.name.compare(0, prefix.size(), prefix) == 0) {
                    ++remaining;
                }
            }
            if (remaining > 0) {
                CajetaArchive::DepSummary nd = d;
                nd.includedEntryCount = remaining;
                newDeps.push_back(nd);
            }
        }
        out.setDeps(std::move(newDeps));
    }

    return writeArchiveOrReport(out, positional[1], "strip");
}

// ---------------------------------------------------------------- merge
int cmdMerge(const std::vector<std::string>& args, const CommonFlags& /*f*/) {
    std::string outName;
    std::string outVersion;
    std::string outKind = "uber";
    bool prefixDeps     = false;
    bool allowCollisions = false;
    std::vector<std::string> positional;
    for (const auto& a : args) {
        if (a.rfind("--name=",    0) == 0) outName    = a.substr(7);
        else if (a.rfind("--version=", 0) == 0) outVersion = a.substr(10);
        else if (a.rfind("--kind=", 0) == 0) {
            outKind = a.substr(7);
            if (outKind != "cja" && outKind != "uber") {
                std::cerr << "cajeta archive merge: --kind must be cja or uber\n";
                return EXIT_USAGE;
            }
        }
        else if (a == "--prefix-deps")      prefixDeps = true;
        else if (a == "--allow-collisions") allowCollisions = true;
        else if (a.rfind("--", 0) == 0) {
            std::cerr << "cajeta archive merge: unknown flag: " << a << "\n";
            return EXIT_USAGE;
        }
        else positional.push_back(a);
    }
    if (positional.size() < 3) {
        std::cerr << "cajeta archive merge: usage: merge <out> <a> <b> [<c>...]\n";
        return EXIT_USAGE;
    }

    std::string outPath = positional[0];
    std::vector<CajetaArchive> inputs;
    for (size_t i = 1; i < positional.size(); ++i) {
        auto load = loadArchiveOrReport(positional[i]);
        if (!load.ok) return load.exitCode;
        inputs.push_back(std::move(load.archive));
    }

    // Resolve output name / version: explicit flag wins; else require
    // every input to agree.
    if (outName.empty()) {
        outName = inputs[0].getName();
        for (size_t i = 1; i < inputs.size(); ++i) {
            if (inputs[i].getName() != outName) {
                std::cerr << "cajeta archive merge: inputs disagree on name "
                          << "(" << outName << " vs " << inputs[i].getName()
                          << "); pass --name=<...>\n";
                return EXIT_USAGE;
            }
        }
    }
    if (outVersion.empty()) {
        outVersion = inputs[0].getVersion();
        for (size_t i = 1; i < inputs.size(); ++i) {
            if (inputs[i].getVersion() != outVersion) {
                std::cerr << "cajeta archive merge: inputs disagree on version "
                          << "(" << outVersion << " vs " << inputs[i].getVersion()
                          << "); pass --version=<...>\n";
                return EXIT_USAGE;
            }
        }
    }

    CajetaArchive::Kind kind = (outKind == "cja")
        ? CajetaArchive::Kind::Cja
        : CajetaArchive::Kind::Uber;
    CajetaArchive out(outName, outVersion, kind);

    // Collect entries left-to-right with collision handling. Map
    // tracks position in out's pending-entries list so we can replace
    // in place under --allow-collisions.
    std::unordered_map<std::string, size_t> sourceFor;  // entry name → input index that produced it
    std::vector<CajetaArchiveEntry> staged;
    std::unordered_map<std::string, size_t> stagedAt;   // entry name → index in staged

    for (size_t i = 0; i < inputs.size(); ++i) {
        const auto& in = inputs[i];
        std::string prefix;
        if (prefixDeps) {
            prefix = "deps/" + in.getName() + "-" + in.getVersion() + "/";
        }
        for (const auto& e : in.getEntries()) {
            std::string name = prefix.empty() ? e.name : (prefix + e.name);
            auto it = stagedAt.find(name);
            if (it != stagedAt.end()) {
                if (!allowCollisions) {
                    std::cerr << "cajeta archive merge: collision on entry "
                              << name << " (in "
                              << positional[1 + sourceFor[name]] << " and "
                              << positional[1 + i]
                              << "); pass --allow-collisions to take the rightmost\n";
                    return EXIT_COLLISION;
                }
                staged[it->second] = cloneEntry(e);
                staged[it->second].name = name;
                sourceFor[name] = i;
            } else {
                CajetaArchiveEntry clone = cloneEntry(e);
                clone.name = name;
                stagedAt[name] = staged.size();
                sourceFor[name] = i;
                staged.push_back(std::move(clone));
            }
        }
    }
    for (auto& e : staged) out.addEntry(std::move(e));

    // Build the deps array when --prefix-deps is set: one summary per
    // input archive describing what it contributed.
    if (prefixDeps && kind == CajetaArchive::Kind::Uber) {
        std::vector<CajetaArchive::DepSummary> deps;
        for (const auto& in : inputs) {
            CajetaArchive::DepSummary d;
            d.name    = in.getName();
            d.version = in.getVersion();
            d.includedEntryCount = (uint32_t) in.getEntries().size();
            deps.push_back(std::move(d));
        }
        out.setDeps(std::move(deps));
    }

    return writeArchiveOrReport(out, outPath, "merge");
}

// ---------------------------------------------------------------- sign / verify-sig
// RAII wrappers around OpenSSL handles. ed25519 sign/verify needs
// EVP_PKEY (parsed from PEM) and EVP_MD_CTX (the digest/sign context).
struct EvpPkeyDeleter   { void operator()(EVP_PKEY* p)   const { if (p) EVP_PKEY_free(p); } };
struct EvpMdCtxDeleter  { void operator()(EVP_MD_CTX* c) const { if (c) EVP_MD_CTX_free(c); } };
struct BioDeleter       { void operator()(BIO* b)        const { if (b) BIO_free(b); } };
using PkeyPtr  = std::unique_ptr<EVP_PKEY,   EvpPkeyDeleter>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;
using BioPtr   = std::unique_ptr<BIO,        BioDeleter>;

// Surface the most recent OpenSSL error to stderr, then drain the
// error queue. Called after every failing EVP/PEM call so the user
// sees libcrypto's reason string ("bad signature", "no start line",
// ...) instead of a generic message.
void emitOpenSSLError(const char* prefix) {
    unsigned long err = ERR_get_error();
    if (err == 0) {
        std::cerr << prefix << ": OpenSSL error (unspecified)\n";
        return;
    }
    char buf[256];
    ERR_error_string_n(err, buf, sizeof(buf));
    std::cerr << prefix << ": " << buf << "\n";
    while (ERR_get_error() != 0) {}  // drain
}

int cmdSign(const std::vector<std::string>& args, const CommonFlags& /*f*/) {
    std::string keyPath;
    std::string outPath;
    std::string keyId;
    std::vector<std::string> positional;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--key") {
            if (i + 1 >= args.size()) {
                std::cerr << "cajeta archive sign: --key requires a path\n";
                return EXIT_USAGE;
            }
            keyPath = args[++i];
        } else if (a.rfind("--key=", 0) == 0) {
            keyPath = a.substr(6);
        } else if (a == "--out") {
            if (i + 1 >= args.size()) {
                std::cerr << "cajeta archive sign: --out requires a path\n";
                return EXIT_USAGE;
            }
            outPath = args[++i];
        } else if (a.rfind("--out=", 0) == 0) {
            outPath = a.substr(6);
        } else if (a == "--key-id") {
            if (i + 1 >= args.size()) {
                std::cerr << "cajeta archive sign: --key-id requires a value\n";
                return EXIT_USAGE;
            }
            keyId = args[++i];
        } else if (a.rfind("--key-id=", 0) == 0) {
            keyId = a.substr(9);
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "cajeta archive sign: unknown flag: " << a << "\n";
            return EXIT_USAGE;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.empty()) {
        std::cerr << "cajeta archive sign: missing <archive>\n";
        return EXIT_USAGE;
    }
    if (keyPath.empty()) {
        std::cerr << "cajeta archive sign: --key <ed25519.pem> is required\n";
        return EXIT_USAGE;
    }
    if (outPath.empty()) {
        if (positional[0] == "-") {
            std::cerr << "cajeta archive sign: --out is required when archive is stdin\n";
            return EXIT_USAGE;
        }
        outPath = positional[0] + ".sig";
    }

    std::vector<uint8_t> archiveBytes;
    try {
        archiveBytes = readPathOrStdin(positional[0]);
    } catch (const std::exception& e) {
        std::cerr << "cajeta archive sign: " << e.what() << "\n";
        return EXIT_NOT_FOUND;
    }

    // Load the ed25519 private key from PEM. OpenSSL handles
    // PKCS#8-wrapped ed25519 keys directly through EVP_PKEY.
    BioPtr keyBio(BIO_new_file(keyPath.c_str(), "r"));
    if (!keyBio) {
        std::cerr << "cajeta archive sign: cannot open key: " << keyPath << "\n";
        return EXIT_NOT_FOUND;
    }
    PkeyPtr pkey(PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));
    if (!pkey) {
        emitOpenSSLError("cajeta archive sign: PEM parse failed");
        return EXIT_USAGE;
    }
    if (EVP_PKEY_id(pkey.get()) != EVP_PKEY_ED25519) {
        std::cerr << "cajeta archive sign: key is not ed25519 "
                     "(expected -----BEGIN PRIVATE KEY----- with an Ed25519 algorithm)\n";
        return EXIT_USAGE;
    }

    // Single-shot sign — ed25519's EVP path uses DigestSign without a
    // separate digest stage (the curve incorporates hashing itself).
    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) { emitOpenSSLError("cajeta archive sign"); return EXIT_IO; }
    if (EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
        emitOpenSSLError("cajeta archive sign: DigestSignInit");
        return EXIT_IO;
    }
    size_t sigLen = 0;
    if (EVP_DigestSign(ctx.get(), nullptr, &sigLen,
                       archiveBytes.data(), archiveBytes.size()) != 1) {
        emitOpenSSLError("cajeta archive sign: probe signature length");
        return EXIT_IO;
    }
    std::vector<uint8_t> sig(sigLen);
    if (EVP_DigestSign(ctx.get(), sig.data(), &sigLen,
                       archiveBytes.data(), archiveBytes.size()) != 1) {
        emitOpenSSLError("cajeta archive sign: sign");
        return EXIT_IO;
    }
    sig.resize(sigLen);

    std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "cajeta archive sign: cannot open output: " << outPath << "\n";
        return EXIT_IO;
    }
    out.write((const char*) sig.data(), (std::streamsize) sig.size());
    if (!out) {
        std::cerr << "cajeta archive sign: short write to " << outPath << "\n";
        return EXIT_IO;
    }

    // Phase 10: write the key-id sidecar so the launcher's
    // signature-verify path can resolve the matching public key.
    if (!keyId.empty()) {
        std::ofstream kidOut(outPath + ".keyid", std::ios::trunc);
        if (kidOut) kidOut << keyId << "\n";
    }
    return EXIT_OK;
}

int cmdVerifySig(const std::vector<std::string>& args, const CommonFlags& f) {
    std::string pubkeyPath;
    std::string sigPath;
    std::vector<std::string> positional;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--pubkey") {
            if (i + 1 >= args.size()) {
                std::cerr << "cajeta archive verify-sig: --pubkey requires a path\n";
                return EXIT_USAGE;
            }
            pubkeyPath = args[++i];
        } else if (a.rfind("--pubkey=", 0) == 0) {
            pubkeyPath = a.substr(9);
        } else if (a == "--sig") {
            if (i + 1 >= args.size()) {
                std::cerr << "cajeta archive verify-sig: --sig requires a path\n";
                return EXIT_USAGE;
            }
            sigPath = args[++i];
        } else if (a.rfind("--sig=", 0) == 0) {
            sigPath = a.substr(6);
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "cajeta archive verify-sig: unknown flag: " << a << "\n";
            return EXIT_USAGE;
        } else {
            positional.push_back(a);
        }
    }
    if (positional.empty()) {
        std::cerr << "cajeta archive verify-sig: missing <archive>\n";
        return EXIT_USAGE;
    }
    if (pubkeyPath.empty()) {
        std::cerr << "cajeta archive verify-sig: --pubkey <pem> is required\n";
        return EXIT_USAGE;
    }
    if (sigPath.empty()) {
        if (positional[0] == "-") {
            std::cerr << "cajeta archive verify-sig: --sig is required when archive is stdin\n";
            return EXIT_USAGE;
        }
        sigPath = positional[0] + ".sig";
    }

    std::vector<uint8_t> archiveBytes;
    std::vector<uint8_t> sigBytes;
    try {
        archiveBytes = readPathOrStdin(positional[0]);
        sigBytes     = readPathOrStdin(sigPath);
    } catch (const std::exception& e) {
        std::cerr << "cajeta archive verify-sig: " << e.what() << "\n";
        return EXIT_NOT_FOUND;
    }

    BioPtr keyBio(BIO_new_file(pubkeyPath.c_str(), "r"));
    if (!keyBio) {
        std::cerr << "cajeta archive verify-sig: cannot open pubkey: "
                  << pubkeyPath << "\n";
        return EXIT_NOT_FOUND;
    }
    PkeyPtr pkey(PEM_read_bio_PUBKEY(keyBio.get(), nullptr, nullptr, nullptr));
    if (!pkey) {
        emitOpenSSLError("cajeta archive verify-sig: PEM parse failed");
        return EXIT_USAGE;
    }
    if (EVP_PKEY_id(pkey.get()) != EVP_PKEY_ED25519) {
        std::cerr << "cajeta archive verify-sig: pubkey is not ed25519\n";
        return EXIT_USAGE;
    }

    MdCtxPtr ctx(EVP_MD_CTX_new());
    if (!ctx) { emitOpenSSLError("cajeta archive verify-sig"); return EXIT_IO; }
    if (EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, pkey.get()) != 1) {
        emitOpenSSLError("cajeta archive verify-sig: DigestVerifyInit");
        return EXIT_IO;
    }
    int rv = EVP_DigestVerify(ctx.get(), sigBytes.data(), sigBytes.size(),
                              archiveBytes.data(), archiveBytes.size());
    if (rv == 1) {
        if (!f.quiet) {
            std::cout << "ok: " << positional[0] << " signature valid\n";
        }
        return EXIT_OK;
    }
    // rv == 0 → signature didn't validate; rv < 0 → OpenSSL error.
    // Both surface as EXIT_SIG_INVALID per the spec.
    if (rv < 0) emitOpenSSLError("cajeta archive verify-sig");
    else        std::cerr << "cajeta archive verify-sig: signature invalid\n";
    return EXIT_SIG_INVALID;
}

} // anonymous namespace

// --------------------------------------------------------------------------
int dispatchArchive(int argc, const char* argv[]) {
    if (argc < 3) {
        printArchiveUsage();
        return EXIT_USAGE;
    }
    std::string sub = argv[2];

    // Build the args vector that the per-subcommand handler sees.
    // argv[0] = "cajeta", argv[1] = "archive", argv[2] = subcommand,
    // argv[3..] = subcommand args.
    std::vector<std::string> args;
    args.reserve(argc - 3);
    for (int i = 3; i < argc; ++i) args.emplace_back(argv[i]);

    CommonFlags flags;
    consumeCommonFlags(args, flags);

    if (sub == "list")       return cmdList(args, flags);
    if (sub == "cat")        return cmdCat(args, flags);
    if (sub == "extract")    return cmdExtract(args, flags);
    if (sub == "info")       return cmdInfo(args, flags);
    if (sub == "deps")       return cmdDeps(args, flags);
    if (sub == "verify")     return cmdVerify(args, flags);
    if (sub == "diff")       return cmdDiff(args, flags);
    if (sub == "repack")     return cmdRepack(args, flags);
    if (sub == "strip")      return cmdStrip(args, flags);
    if (sub == "merge")      return cmdMerge(args, flags);
    if (sub == "sign")       return cmdSign(args, flags);
    if (sub == "verify-sig") return cmdVerifySig(args, flags);
    if (sub == "--help" || sub == "-h" || sub == "help") {
        printArchiveUsage();
        return EXIT_OK;
    }

    std::cerr << "cajeta archive: unknown subcommand: " << sub << "\n";
    printArchiveUsage();
    return EXIT_USAGE;
}

} // namespace cajeta
