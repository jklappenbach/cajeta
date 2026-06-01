#include "cajeta/buildtool/BuildToolCommands.h"

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>

#include <cstring>
#include <iostream>
#include <string>
#include <string_view>

namespace cajeta::buildtool {

    namespace {

        // Strip the `--flag=` prefix if present and capture the value.
        // Returns true when `arg` matched the flag form `--name=value`.
        bool match(std::string_view arg,
                   std::string_view name,
                   std::string& value) {
            std::string prefix = "--";
            prefix.append(name.data(), name.size());
            prefix += "=";
            if (arg.size() < prefix.size()) return false;
            if (arg.compare(0, prefix.size(), prefix) != 0) return false;
            value = std::string(arg.substr(prefix.size()));
            return true;
        }

        // `cajeta info` — Phase 0 implementation. Loads the manifest
        // and prints a structured summary. `--properties` (deferred to
        // Phase 1) will print the resolved property set; today we
        // print the unresolved properties block verbatim so the
        // surface is visible end-to-end.
        int infoCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            bool dumpProperties = false;
            bool resolved = false;

            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = std::move(value);
                } else if (arg == "--properties") {
                    dumpProperties = true;
                } else if (arg == "--resolved") {
                    resolved = true;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta info [--manifest=<path>] "
                           "[--properties] [--resolved]\n"
                        << "\n"
                        << "Prints the parsed manifest. Phase 0 prints the "
                           "details block + the names of the other top-level "
                           "blocks.\n"
                        << "\n"
                        << "  --manifest=<path>   Manifest file (default: ./cajeta.json)\n"
                        << "  --properties        Dump the properties block (resolution arrives in Phase 1)\n"
                        << "  --resolved          Dump the resolved manifest (Phase 8)\n";
                    return 0;
                } else {
                    std::cerr << "cajeta info: unknown argument '"
                              << arg << "'\n";
                    return 1;
                }
            }

            (void)resolved;  // Phase 8 surface; accepted today but no-op.

            auto manifest = loadManifestFile(manifestPath);
            if (!manifest) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << manifest.takeError();
                std::cerr << "cajeta info: " << msg << "\n";
                return 1;
            }

            const auto& m = *manifest;
            std::cout << "Manifest: " << m.sourcePath << "\n";
            std::cout << "  name:    " << m.details.name << "\n";
            std::cout << "  version: " << m.details.version << "\n";
            std::cout << "  group:   " << m.details.group() << "\n";
            std::cout << "  library: " << m.details.library() << "\n";
            if (m.details.description) {
                std::cout << "  description: " << *m.details.description << "\n";
            }
            if (m.details.license) {
                std::cout << "  license: " << *m.details.license << "\n";
            }
            if (!m.details.authors.empty()) {
                std::cout << "  authors:\n";
                for (const auto& a : m.details.authors) {
                    std::cout << "    - " << a << "\n";
                }
            }
            if (m.details.cajetaLangVersion) {
                std::cout << "  cajeta-lang-version: "
                          << *m.details.cajetaLangVersion << "\n";
            }

            std::cout << "\nBlocks:\n";
            std::cout << "  properties: " << m.propertiesRaw.size() << " entries\n";
            std::cout << "  settings:   " << m.settingsRaw.size() << " entries\n";
            std::cout << "  actions:    " << m.actionsRaw.size() << " entries\n";
            std::cout << "  plugins:    " << m.pluginsRaw.size() << " entries\n";
            std::cout << "  tasks:      " << m.tasksRaw.size() << " entries\n";

            if (dumpProperties && !m.propertiesRaw.empty()) {
                std::cout << "\nProperties (unresolved):\n";
                llvm::json::Value v(llvm::json::Object(m.propertiesRaw));
                llvm::outs() << llvm::formatv("{0:2}", v) << "\n";
            }

            return 0;
        }

    } // namespace

    bool dispatchBuildTool(int argc, const char* argv[], int* exitCodeOut) {
        if (argc < 2) return false;
        std::string_view cmd = argv[1];

        if (cmd == "info") {
            *exitCodeOut = infoCommand(argc, argv);
            return true;
        }

        // Other build-tool subcommands land in subsequent phases. For
        // now anything not recognized falls through to the compiler.
        return false;
    }

} // namespace cajeta::buildtool
