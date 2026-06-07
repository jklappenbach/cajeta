#include "IdeCommands.h"
#include "EmbeddedPlugin.h"

#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifndef CAJETA_PLUGIN_VERSION
#define CAJETA_PLUGIN_VERSION "0.0.0-unknown"
#endif

namespace cajeta {

namespace {

namespace fs = std::filesystem;

enum : int { EXIT_OK = 0, EXIT_USAGE = 1, EXIT_NONE = 2, EXIT_IO = 8, EXIT_CORRUPT = 6 };

// The top-level directory name inside the plugin distribution zip
// (cajeta-idea/lib/*.jar). Stable across versions — it's the Gradle archive
// base name, not the versioned zip file name — so it's also the installed
// plugin directory name we create / remove / probe.
constexpr const char* kPluginDirName = "cajeta-idea";

// ---------------------------------------------------------------- minimal zip
// Just enough ZIP reader to extract the embedded plugin: scan the End Of
// Central Directory record, walk the central directory, and inflate each
// entry (STORE or raw DEFLATE) via zlib. No ZIP64 (the plugin is ~18 MB and
// every entry is well under 4 GB); a 0xFFFFFFFF size field is rejected.

uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

bool inflateRaw(const uint8_t* src, size_t srcLen,
                std::vector<uint8_t>& out, uint32_t expectedOut) {
    out.resize(expectedOut);
    if (expectedOut == 0) return true;
    z_stream s;
    std::memset(&s, 0, sizeof(s));
    // Negative window bits → raw deflate stream (no zlib/gzip header), which
    // is exactly what ZIP local-header entries carry.
    if (inflateInit2(&s, -MAX_WBITS) != Z_OK) return false;
    s.next_in   = const_cast<Bytef*>(src);
    s.avail_in  = (uInt)srcLen;
    s.next_out  = out.data();
    s.avail_out = (uInt)expectedOut;
    int rc = inflate(&s, Z_FINISH);
    inflateEnd(&s);
    return rc == Z_STREAM_END;
}

// Extract the embedded zip into destRoot. Entries carry their own relative
// paths (e.g. "cajeta-idea/lib/foo.jar"), so the plugin lands at
// destRoot/cajeta-idea/... Returns false on any structural / IO error.
bool extractEmbeddedZip(const fs::path& destRoot, std::string& err) {
    const uint8_t* zip = cajeta_plugin_zip;
    const size_t   n   = cajeta_plugin_zip_len;
    if (n < 22) { err = "embedded plugin zip is empty or truncated"; return false; }

    // Find EOCD (sig 0x06054b50) by scanning backward; comment is usually empty.
    const uint32_t EOCD_SIG = 0x06054b50u;
    size_t eocd = SIZE_MAX;
    size_t scanStart = n >= (22 + 65535) ? n - (22 + 65535) : 0;
    for (size_t i = n - 22 + 1; i-- > scanStart; ) {
        if (rd32(zip + i) == EOCD_SIG) { eocd = i; break; }
    }
    if (eocd == SIZE_MAX) { err = "no end-of-central-directory record"; return false; }

    uint16_t entries = rd16(zip + eocd + 10);
    uint32_t cdOff   = rd32(zip + eocd + 16);
    if (cdOff == 0xFFFFFFFFu) { err = "ZIP64 archives are not supported"; return false; }

    size_t p = cdOff;
    const uint32_t CDH_SIG = 0x02014b50u;
    const uint32_t LFH_SIG = 0x04034b50u;
    for (uint16_t e = 0; e < entries; ++e) {
        if (p + 46 > n || rd32(zip + p) != CDH_SIG) {
            err = "corrupt central directory"; return false;
        }
        uint16_t method   = rd16(zip + p + 10);
        uint32_t compSize = rd32(zip + p + 20);
        uint32_t fullSize = rd32(zip + p + 24);
        uint16_t nameLen  = rd16(zip + p + 28);
        uint16_t extraLen = rd16(zip + p + 30);
        uint16_t cmtLen   = rd16(zip + p + 32);
        uint32_t lfhOff   = rd32(zip + p + 42);
        if (compSize == 0xFFFFFFFFu || fullSize == 0xFFFFFFFFu) {
            err = "ZIP64 entry is not supported"; return false;
        }
        std::string name((const char*)(zip + p + 46), nameLen);
        p += 46 + nameLen + extraLen + cmtLen;

        // Reject path traversal — the embedded zip is ours, but never write
        // outside destRoot regardless.
        if (name.find("..") != std::string::npos) {
            err = "unsafe path in zip: " + name; return false;
        }
        fs::path target = destRoot / fs::path(name);

        if (!name.empty() && name.back() == '/') {        // directory entry
            std::error_code ec;
            fs::create_directories(target, ec);
            continue;
        }

        // Locate the entry data via its LOCAL header (its name/extra lengths
        // can differ from the central directory's).
        if (lfhOff + 30 > n || rd32(zip + lfhOff) != LFH_SIG) {
            err = "corrupt local header for " + name; return false;
        }
        uint16_t lNameLen  = rd16(zip + lfhOff + 26);
        uint16_t lExtraLen = rd16(zip + lfhOff + 28);
        size_t dataOff = lfhOff + 30 + lNameLen + lExtraLen;
        if (dataOff + compSize > n) { err = "entry data out of range: " + name; return false; }

        std::vector<uint8_t> data;
        if (method == 0) {                                // STORE
            data.assign(zip + dataOff, zip + dataOff + compSize);
        } else if (method == 8) {                         // DEFLATE
            if (!inflateRaw(zip + dataOff, compSize, data, fullSize)) {
                err = "inflate failed for " + name; return false;
            }
        } else {
            err = "unsupported compression method for " + name; return false;
        }

        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) { err = "cannot write " + target.string(); return false; }
        if (!data.empty()) out.write((const char*)data.data(), (std::streamsize)data.size());
        if (!out) { err = "write error on " + target.string(); return false; }
    }
    return true;
}

// ---------------------------------------------------------------- IDE layout

fs::path jetbrainsConfigBase() {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA")) return fs::path(appdata) / "JetBrains";
    return {};
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / "Library" / "Application Support" / "JetBrains";
    return {};
#else
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / "JetBrains";
    if (const char* home = std::getenv("HOME")) return fs::path(home) / ".config" / "JetBrains";
    return {};
#endif
}

// The plugins directory for a given product dir name (e.g. "IntelliJIdea2025.2").
// Layout differs per OS (verified against a live JetBrains install):
//   Windows: %APPDATA%\JetBrains\<product>\plugins
//   macOS:   ~/Library/Application Support/JetBrains/<product>/plugins
//   Linux:   ~/.local/share/JetBrains/<product>      (no "plugins" subdir)
fs::path pluginsDirFor(const std::string& product) {
#if defined(_WIN32)
    if (const char* appdata = std::getenv("APPDATA"))
        return fs::path(appdata) / "JetBrains" / product / "plugins";
    return {};
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / "Library" / "Application Support" / "JetBrains" / product / "plugins";
    return {};
#else
    fs::path dataBase;
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) dataBase = fs::path(xdg);
    else if (const char* home = std::getenv("HOME")) dataBase = fs::path(home) / ".local" / "share";
    else return {};
    return dataBase / "JetBrains" / product;
#endif
}

// D11: IDEA only. Match Ultimate ("IntelliJIdea<ver>") and Community
// ("IdeaIC<ver>" / "IdeaIU<ver>"); skip CLion / PyCharm / Rider / WebStorm etc.
bool isIdeaProduct(const std::string& name) {
    auto starts = [&](const char* pfx) { return name.rfind(pfx, 0) == 0; };
    return starts("IntelliJIdea") || starts("IdeaIC") || starts("IdeaIU");
}

// Detected IDEA product dir names (e.g. "IntelliJIdea2025.2"), sorted.
std::vector<std::string> detectIdeaProducts() {
    std::vector<std::string> out;
    fs::path base = jetbrainsConfigBase();
    std::error_code ec;
    if (base.empty() || !fs::is_directory(base, ec)) return out;
    for (const auto& ent : fs::directory_iterator(base, ec)) {
        if (!ent.is_directory()) continue;
        std::string name = ent.path().filename().string();
        if (isIdeaProduct(name)) out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool pluginBundled() { return cajeta_plugin_zip_len > 0; }

// ---------------------------------------------------------------- subcommands

void printIdeUsage() {
    std::cerr <<
        "Usage: cajeta ide <command> [options]\n"
        "\n"
        "Manage the bundled IntelliJ IDEA plugin (embedded in this binary).\n"
        "\n"
        "Commands:\n"
        "  install      Install the bundled plugin into every detected IDEA.\n"
        "  uninstall    Remove the bundled plugin from every detected IDEA.\n"
        "  list         List detected IDEA installs and plugin status.\n"
        "\n"
        "Options:\n"
        "  --plugins-dir=<path>   Target this plugins directory explicitly\n"
        "                         (skips auto-detection).\n"
        "  --help, -h             This message.\n"
        "\n"
        "After install, restart IntelliJ IDEA for the plugin to load.\n";
}

// Pull a --plugins-dir=<path> override out of args if present.
bool takePluginsDirOverride(std::vector<std::string>& args, fs::path& out) {
    const std::string pfx = "--plugins-dir=";
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (it->rfind(pfx, 0) == 0) { out = it->substr(pfx.size()); args.erase(it); return true; }
    }
    return false;
}

// Install into one plugins dir. Idempotent: an existing cajeta-idea dir is
// removed first so a reinstall replaces rather than merges.
int installInto(const fs::path& pluginsDir) {
    std::error_code ec;
    fs::create_directories(pluginsDir, ec);
    if (ec) {
        std::cerr << "cajeta ide: cannot create " << pluginsDir << ": " << ec.message() << "\n";
        return EXIT_IO;
    }
    fs::path existing = pluginsDir / kPluginDirName;
    if (fs::exists(existing, ec)) fs::remove_all(existing, ec);
    std::string err;
    if (!extractEmbeddedZip(pluginsDir, err)) {
        std::cerr << "cajeta ide: failed to extract plugin into " << pluginsDir
                  << ": " << err << "\n";
        return EXIT_CORRUPT;
    }
    std::cout << "  installed -> " << existing.string() << "\n";
    return EXIT_OK;
}

int cmdInstall(std::vector<std::string> args) {
    if (!pluginBundled()) {
        std::cerr << "cajeta ide: this build has no bundled plugin "
                     "(it was compiled before the IDEA plugin was built).\n";
        return EXIT_NONE;
    }
    fs::path override;
    if (takePluginsDirOverride(args, override)) {
        std::cout << "Installing Cajeta plugin " << CAJETA_PLUGIN_VERSION
                  << " into " << override.string() << "\n";
        int rc = installInto(override);
        if (rc == EXIT_OK) std::cout << "Done. Restart IntelliJ IDEA to load the plugin.\n";
        return rc;
    }

    std::vector<std::string> products = detectIdeaProducts();
    if (products.empty()) {
        std::cerr << "cajeta ide: no IntelliJ IDEA installation detected under "
                  << jetbrainsConfigBase().string() << ".\n"
                  << "  Launch IDEA once so it creates its config dir, or pass "
                     "--plugins-dir=<path>.\n";
        return EXIT_NONE;
    }
    std::cout << "Installing Cajeta plugin " << CAJETA_PLUGIN_VERSION << ":\n";
    int worst = EXIT_OK;
    for (const auto& product : products) {
        std::cout << product << ":\n";
        int rc = installInto(pluginsDirFor(product));
        if (rc != EXIT_OK) worst = rc;
    }
    if (worst == EXIT_OK) std::cout << "Done. Restart IntelliJ IDEA to load the plugin.\n";
    return worst;
}

int cmdUninstall(std::vector<std::string> args) {
    fs::path override;
    std::vector<fs::path> dirs;
    if (takePluginsDirOverride(args, override)) {
        dirs.push_back(override);
    } else {
        for (const auto& product : detectIdeaProducts()) dirs.push_back(pluginsDirFor(product));
    }
    if (dirs.empty()) {
        std::cerr << "cajeta ide: no IntelliJ IDEA installation detected.\n";
        return EXIT_NONE;
    }
    int removed = 0;
    for (const auto& pluginsDir : dirs) {
        fs::path target = pluginsDir / kPluginDirName;
        std::error_code ec;
        if (fs::exists(target, ec)) {
            fs::remove_all(target, ec);
            if (ec) { std::cerr << "  failed to remove " << target.string() << ": " << ec.message() << "\n"; }
            else { std::cout << "  removed -> " << target.string() << "\n"; ++removed; }
        }
    }
    if (removed == 0) std::cout << "No bundled Cajeta plugin was installed.\n";
    else std::cout << "Removed from " << removed << " location(s). Restart IDEA.\n";
    return EXIT_OK;
}

int cmdList(std::vector<std::string> args) {
    fs::path override;
    if (takePluginsDirOverride(args, override)) {
        bool present = fs::exists(override / kPluginDirName);
        std::cout << override.string() << ": "
                  << (present ? "installed" : "not installed") << "\n";
        return EXIT_OK;
    }
    std::cout << "Bundled plugin: "
              << (pluginBundled() ? std::string(CAJETA_PLUGIN_VERSION) : std::string("none"))
              << "\n";
    std::vector<std::string> products = detectIdeaProducts();
    if (products.empty()) {
        std::cout << "No IntelliJ IDEA installation detected under "
                  << jetbrainsConfigBase().string() << ".\n";
        return EXIT_OK;
    }
    std::cout << "Detected IntelliJ IDEA:\n";
    for (const auto& product : products) {
        fs::path target = pluginsDirFor(product) / kPluginDirName;
        std::cout << "  " << product << "  ["
                  << (fs::exists(target) ? "installed" : "not installed") << "]\n";
    }
    return EXIT_OK;
}

} // namespace

int dispatchIde(int argc, const char* argv[]) {
    std::vector<std::string> args;
    for (int i = 3; i < argc; ++i) args.emplace_back(argv[i]);

    std::string sub = (argc >= 3) ? argv[2] : "";
    if (sub.empty() || sub == "--help" || sub == "-h") { printIdeUsage(); return sub.empty() ? EXIT_USAGE : EXIT_OK; }
    if (sub == "install")   return cmdInstall(std::move(args));
    if (sub == "uninstall") return cmdUninstall(std::move(args));
    if (sub == "list")      return cmdList(std::move(args));

    std::cerr << "cajeta ide: unknown command '" << sub << "'\n\n";
    printIdeUsage();
    return EXIT_USAGE;
}

} // namespace cajeta
