// `cajeta stdlib <list|extract <dir>>` — see StdlibCommands.h.

#include "StdlibCommands.h"

#include "../compile/StdlibEmbedded.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

// Stamped globally at configure time (top-level CMakeLists.txt); the
// fallbacks mirror main.cpp so a bare build still produces a marker.
#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif
#ifndef CAJETA_GIT_HASH
#define CAJETA_GIT_HASH "unknown"
#endif

namespace cajeta {

    namespace {

        namespace fs = std::filesystem;

        void printStdlibUsage() {
            std::cerr <<
                "Usage: cajeta stdlib <verb> [args...]\n"
                "\n"
                "Verbs:\n"
                "  list             print the embedded stdlib source set, one\n"
                "                   package-relative path per line, to stdout\n"
                "  extract <dir>    write the embedded stdlib sources under <dir>,\n"
                "                   preserving package paths, plus a\n"
                "                   .cajeta-stdlib.json identity marker (compiler\n"
                "                   version + git hash + file count) for cache\n"
                "                   keying\n";
        }

        int listStdlib() {
            for (size_t i = 0; i < stdlib::g_fileCount; ++i) {
                std::cout << stdlib::g_files[i].relativePath << "\n";
            }
            return 0;
        }

        // JSON string escaping for the marker. Version/hash strings are
        // configure-time constants, but a fork's VERSION file could hold
        // anything printable.
        std::string jsonEscape(const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                switch (c) {
                    case '"':  out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\n': out += "\\n";  break;
                    case '\t': out += "\\t";  break;
                    default:   out += c;      break;
                }
            }
            return out;
        }

        int extractStdlib(const std::string& targetDir) {
            std::error_code ec;
            fs::create_directories(targetDir, ec);
            if (ec) {
                std::cerr << "cajeta stdlib extract: cannot create " << targetDir
                          << ": " << ec.message() << "\n";
                return 1;
            }

            // Write each embedded source to <dir>/<relativePath>. Existing
            // extracted files are overwritten (re-extraction refreshes them);
            // nothing else in the target directory is touched.
            for (size_t i = 0; i < stdlib::g_fileCount; ++i) {
                const auto& f = stdlib::g_files[i];
                fs::path dest = fs::path(targetDir) / f.relativePath;
                fs::create_directories(dest.parent_path(), ec);
                if (ec) {
                    std::cerr << "cajeta stdlib extract: cannot create "
                              << dest.parent_path().string() << ": "
                              << ec.message() << "\n";
                    return 1;
                }
                std::ofstream out(dest, std::ios::binary | std::ios::trunc);
                if (!out) {
                    std::cerr << "cajeta stdlib extract: cannot write "
                              << dest.string() << "\n";
                    return 1;
                }
                out.write(f.content, (std::streamsize) f.contentBytes);
                if (!out) {
                    std::cerr << "cajeta stdlib extract: short write to "
                              << dest.string() << "\n";
                    return 1;
                }
            }

            // Identity marker LAST — its presence with a matching fileCount
            // vouches for a complete extraction, so the plugin can both key
            // its cache on the producing compiler and detect a torn tree.
            fs::path marker = fs::path(targetDir) / ".cajeta-stdlib.json";
            std::ofstream m(marker, std::ios::binary | std::ios::trunc);
            m << "{\n"
              << "  \"version\": \"" << jsonEscape(CAJETA_VERSION) << "\",\n"
              << "  \"gitHash\": \"" << jsonEscape(CAJETA_GIT_HASH) << "\",\n"
              << "  \"fileCount\": " << stdlib::g_fileCount << "\n"
              << "}\n";
            if (!m) {
                std::cerr << "cajeta stdlib extract: cannot write "
                          << marker.string() << "\n";
                return 1;
            }

            std::cerr << "cajeta stdlib extract: wrote " << stdlib::g_fileCount
                      << " files to " << targetDir << "\n";
            return 0;
        }

    } // anonymous namespace

    int dispatchStdlib(int argc, const char* argv[]) {
        if (argc < 3) {
            printStdlibUsage();
            return 1;
        }
        std::string verb = argv[2];
        if (verb == "list") {
            return listStdlib();
        }
        if (verb == "extract") {
            if (argc < 4) {
                std::cerr << "cajeta stdlib extract: usage: extract <dir>\n";
                return 1;
            }
            return extractStdlib(argv[3]);
        }
        printStdlibUsage();
        return 1;
    }

} // namespace cajeta
