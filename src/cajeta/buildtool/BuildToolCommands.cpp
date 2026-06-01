#include "cajeta/buildtool/BuildToolCommands.h"

#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
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

        // `cajeta info` — loads the manifest, optionally resolves
        // properties, prints a structured summary. The summary is the
        // load-bearing diagnostic for "did my manifest parse the way I
        // think it did" — every block visible, every override applied.
        int infoCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string lockfilePath = "./cajeta.lock";
            bool dumpProperties = false;
            bool resolved = false;
            bool writeLock = false;
            bool checkLock = false;
            PropertyOverrides overrides;
            loadEnvOverrides(overrides);

            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = std::move(value);
                } else if (arg == "--properties") {
                    dumpProperties = true;
                } else if (arg == "--resolved") {
                    resolved = true;
                } else if (arg == "-P" && i + 1 < argc) {
                    auto parsed = parseCliOverride(argv[++i]);
                    if (!parsed) {
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << parsed.takeError();
                        std::cerr << "cajeta info: " << msg << "\n";
                        return 1;
                    }
                    overrides.cli[parsed->first] = parsed->second;
                } else if (match(arg, "property", value)) {
                    auto parsed = parseCliOverride(value);
                    if (!parsed) {
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << parsed.takeError();
                        std::cerr << "cajeta info: " << msg << "\n";
                        return 1;
                    }
                    overrides.cli[parsed->first] = parsed->second;
                } else if (match(arg, "flavor", value)) {
                    overrides.flavor = value;
                } else if (match(arg, "profile", value)) {
                    overrides.profile = value;
                } else if (match(arg, "lockfile", value)) {
                    lockfilePath = std::move(value);
                } else if (arg == "--write-lockfile") {
                    writeLock = true;
                } else if (arg == "--check-lockfile") {
                    checkLock = true;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta info [options]\n"
                        << "\n"
                        << "Prints the parsed manifest.\n"
                        << "\n"
                        << "  --manifest=<path>      Manifest file (default: ./cajeta.json)\n"
                        << "  --lockfile=<path>      Lockfile path (default: ./cajeta.lock)\n"
                        << "  --properties           Dump the resolved property set\n"
                        << "  --resolved             Dump the fully-resolved manifest (Phase 8)\n"
                        << "  --write-lockfile       Write cajeta.lock with resolved state\n"
                        << "  --check-lockfile       Verify manifest matches recorded checksum\n"
                        << "  -P NAME=VALUE          Override a property for this invocation\n"
                        << "  --property=NAME=VALUE  Long form of -P\n"
                        << "  --flavor=NAME          Override active build flavor\n"
                        << "  --profile=NAME         Override active profile\n";
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

            if (dumpProperties || writeLock || checkLock) {
                auto resolvedProps = resolveProperties(m, overrides);
                if (!resolvedProps) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << resolvedProps.takeError();
                    std::cerr << "cajeta info: " << msg << "\n";
                    return 1;
                }

                if (dumpProperties) {
                    std::cout << "\nProperties (resolved):\n";
                    for (const auto& name : resolvedProps->resolutionOrder) {
                        std::cout << "  " << name << " = "
                                  << resolvedProps->values[name] << "\n";
                    }
                }

                if (writeLock || checkLock) {
                    auto srcBuf = llvm::MemoryBuffer::getFile(manifestPath);
                    if (!srcBuf) {
                        std::cerr << "cajeta info: cannot read manifest source: "
                                  << srcBuf.getError().message() << "\n";
                        return 1;
                    }
                    std::string manifestSource = (*srcBuf)->getBuffer().str();

                    if (checkLock) {
                        auto existing = readLockfile(lockfilePath);
                        if (!existing) {
                            std::string msg;
                            llvm::raw_string_ostream os(msg);
                            os << existing.takeError();
                            std::cerr << "cajeta info: " << msg << "\n";
                            return 1;
                        }
                        auto drift = checkDrift(*existing, manifestSource);
                        if (drift.changed) {
                            std::cout << "\nLockfile drift detected:\n"
                                      << "  recorded: " << drift.oldChecksum << "\n"
                                      << "  current:  " << drift.newChecksum << "\n";
                            return 1;
                        }
                        std::cout << "\nLockfile up to date ("
                                  << drift.oldChecksum << ")\n";
                    }

                    if (writeLock) {
                        Lockfile lf = composeLockfile(
                            m, manifestSource, *resolvedProps, nowIsoUtc());
                        if (auto e = writeLockfile(lockfilePath, lf)) {
                            std::string msg;
                            llvm::raw_string_ostream os(msg);
                            os << e;
                            std::cerr << "cajeta info: " << msg << "\n";
                            return 1;
                        }
                        std::cout << "\nLockfile written: " << lockfilePath << "\n";
                    }
                }
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
