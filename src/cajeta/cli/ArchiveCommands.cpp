#include "ArchiveCommands.h"

#include "../compile/CajetaArchive.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
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
        "  verify <archive>                    Structural integrity check.\n"
        "  diff <a.cja> <b.cja>                Entry-by-entry diff.\n"
        "\n"
        "Global flags (apply to every subcommand):\n"
        "  --json                              Machine-readable JSON output.\n"
        "  --quiet, -q                         Suppress non-error output.\n"
        "\n"
        "Spec: cajeta-docs/ArchiveManagement.md\n";
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
        r.archive = CajetaArchive::readFrom(path);
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
    if (args.empty()) {
        std::cerr << "cajeta archive verify: missing <archive>\n";
        return EXIT_USAGE;
    }
    // The readFrom path already does the heavy lifting: it validates
    // the magic, format version, header bounds, decompresses the
    // manifest, parses it, walks every entry, decompresses each
    // entry's payload, and confirms no truncation. We re-run it here
    // and add the duplicate-name + manifest-required-field checks
    // that readFrom doesn't enforce.
    auto load = loadArchiveOrReport(args[0]);
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

    if (!f.quiet) {
        std::cout << "ok: " << args[0] << " (" << load.archive.getEntries().size()
                  << " entries, kind=" << archiveKindName(load.archive.getKind())
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

    if (sub == "list")     return cmdList(args, flags);
    if (sub == "cat")      return cmdCat(args, flags);
    if (sub == "extract")  return cmdExtract(args, flags);
    if (sub == "info")     return cmdInfo(args, flags);
    if (sub == "deps")     return cmdDeps(args, flags);
    if (sub == "verify")   return cmdVerify(args, flags);
    if (sub == "diff")     return cmdDiff(args, flags);
    if (sub == "--help" || sub == "-h" || sub == "help") {
        printArchiveUsage();
        return EXIT_OK;
    }

    std::cerr << "cajeta archive: unknown subcommand: " << sub << "\n";
    printArchiveUsage();
    return EXIT_USAGE;
}

} // namespace cajeta
