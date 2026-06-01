#include "cajeta/buildtool/BuildToolCommands.h"

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/InitTemplates.h"
#include "cajeta/buildtool/JsonC.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/ManifestEditor.h"
#include "cajeta/buildtool/Melt.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/Task.h"
#include "cajeta/buildtool/TaskRunner.h"
#include "cajeta/buildtool/Upgrader.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>

#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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
            bool resolveTime = false;
            bool showMelts = false;
            bool showMeltTree = false;
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
                } else if (arg == "--resolve-time") {
                    resolveTime = true;
                } else if (arg == "--melts") {
                    showMelts = true;
                } else if (arg == "--melt-tree") {
                    showMeltTree = true;
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
                        << "  --resolve-time         Run resolver and print per-phase wall-clock\n"
                        << "  --melts                Show imported melts + which dep each one provided\n"
                        << "  --melt-tree            Show the transitive melt graph (post-order)\n"
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

            if (resolveTime) {
                std::string projectRoot =
                    std::filesystem::path(manifestPath)
                        .parent_path().string();
                if (projectRoot.empty()) projectRoot = ".";

                ResolverTimings timings;
                auto resolved = resolveProjectDependencies(
                    m, projectRoot, std::nullopt, &timings);
                if (!resolved) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << resolved.takeError();
                    std::cerr << "cajeta info: resolve-time: "
                              << msg << "\n";
                    return 1;
                }
                auto us = [](ResolverTimings::Duration d) {
                    return d.count();
                };
                std::cout << "\nResolver timings:\n"
                          << "  total:           "
                          << us(timings.total) << " us\n"
                          << "  deps resolved:   "
                          << timings.depsResolved << "\n"
                          << "  MVS iterations:  "
                          << timings.mvsIterations << "\n"
                          << "  listVersions:    "
                          << us(timings.listVersions) << " us across "
                          << timings.listVersionsCalls << " call(s)\n"
                          << "  fetch:           "
                          << us(timings.fetch) << " us across "
                          << timings.fetchCalls << " call(s)\n"
                          << "  fetchManifest:   "
                          << us(timings.fetchManifest) << " us across "
                          << timings.fetchManifestCalls << " call(s)\n";
            }

            if (showMelts || showMeltTree) {
                std::string projectRoot =
                    std::filesystem::path(manifestPath)
                        .parent_path().string();
                if (projectRoot.empty()) projectRoot = ".";

                auto repoSpecs = parseRepositories(m);
                if (!repoSpecs) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << repoSpecs.takeError();
                    std::cerr << "cajeta info: " << msg << "\n";
                    return 1;
                }
                std::string downloadStage =
                    (std::filesystem::path(projectRoot) / ".cajeta" /
                     "cache" / "downloads").string();
                auto repos = buildRepositories(*repoSpecs, downloadStage);
                if (!repos) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << repos.takeError();
                    std::cerr << "cajeta info: " << msg << "\n";
                    return 1;
                }
                ArtifactCache cache(projectRoot);
                auto melts = resolveMelts(m, *repos, cache);
                if (!melts) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << melts.takeError();
                    std::cerr << "cajeta info: " << msg << "\n";
                    return 1;
                }

                if (showMelts) {
                    std::cout << "\nImported melts:\n";
                    if (melts->resolvedMelts.empty()) {
                        std::cout << "  (none declared in settings.melts)\n";
                    }
                    for (const auto& r : melts->resolvedMelts) {
                        std::cout << "  - " << r.name << "@" << r.version
                                  << " (from " << r.resolvedFromRepo
                                  << ")\n";
                    }
                    if (!melts->depConstraints.empty()) {
                        std::cout << "\nConstraints provided by melts:\n";
                        for (const auto& [name, constraint] :
                                 melts->depConstraints) {
                            std::cout << "  " << name << " = "
                                      << constraint << "  (from "
                                      << melts->depProvidedBy[name]
                                      << ")\n";
                        }
                    }
                }

                if (showMeltTree) {
                    std::cout << "\nMelt tree (post-order):\n";
                    if (melts->resolvedMelts.empty()) {
                        std::cout << "  (none)\n";
                    }
                    for (const auto& r : melts->resolvedMelts) {
                        std::cout << "  " << r.name << "@" << r.version
                                  << "\n";
                        for (const auto& t : r.transitiveMelts) {
                            std::cout << "    └─ " << t.name
                                      << "@" << t.version << "\n";
                        }
                    }
                }
            }

            return 0;
        }

        // `cajeta init [<type>] [<dir>]` — write an archetype
        // template to disk. The archetype source lives in
        // samples/buildtool/<type>/; CMake embeds those bytes at
        // build time so the binary is self-contained (no repo lookup
        // at runtime). See cajeta-docs/BuildTool.md "Project shapes".
        int initCommand(int argc, const char* argv[]) {
            std::string templateName = "basic";
            std::string destDir = ".";
            bool force = false;
            bool listOnly = false;

            // Positional parsing: first non-flag is the template
            // name, second is the destination directory. Both
            // optional; defaults are `basic` + `.`.
            int positional = 0;
            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                if (arg == "--force" || arg == "-f") {
                    force = true;
                } else if (arg == "--list") {
                    listOnly = true;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta init [<type>] [<dir>] [options]\n"
                        << "\n"
                        << "Write an archetype template to disk.\n"
                        << "\n"
                        << "  <type>     Archetype name (default: basic)\n"
                        << "  <dir>      Destination directory (default: .)\n"
                        << "  --force    Overwrite existing files\n"
                        << "  --list     List available archetypes and exit\n"
                        << "\n"
                        << "Available archetypes:\n";
                    for (const auto& name : availableInitTemplates()) {
                        std::cout << "  " << name << "\n";
                    }
                    return 0;
                } else if (!arg.empty() && arg[0] == '-') {
                    std::cerr << "cajeta init: unknown argument '"
                              << arg << "'\n";
                    return 1;
                } else if (positional == 0) {
                    templateName = std::string(arg);
                    ++positional;
                } else if (positional == 1) {
                    destDir = std::string(arg);
                    ++positional;
                } else {
                    std::cerr << "cajeta init: unexpected positional argument '"
                              << arg << "'\n";
                    return 1;
                }
            }

            if (listOnly) {
                for (const auto& name : availableInitTemplates()) {
                    std::cout << name << "\n";
                }
                return 0;
            }

            auto result = instantiateInitTemplate(templateName, destDir, force);
            if (!result) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << result.takeError();
                std::cerr << msg << "\n";
                return 1;
            }

            std::cout << "Initialized '" << templateName
                      << "' archetype in " << destDir << ":\n";
            for (const auto& p : result->filesWritten) {
                std::cout << "  " << p << "\n";
            }
            std::cout
                << "\nNext steps:\n"
                << "  1. Edit cajeta.json's `details.name` to your package name.\n"
                << "  2. Rename `src/main/cajeta/com/example/...` to match.\n"
                << "  3. cajeta tasks       # list available tasks\n"
                << "  4. cajeta task <name> --show    # inspect a task\n";
            return 0;
        }

        // Common helpers for `cajeta add` / `cajeta remove`: read +
        // write the manifest file as bytes, no JSONC normalization
        // (those operations want comment + format preservation).
        bool readFileBytes(const std::string& path, std::string& out) {
            std::ifstream in(path, std::ios::binary);
            if (!in) return false;
            std::ostringstream buf;
            buf << in.rdbuf();
            out = buf.str();
            return true;
        }

        bool writeFileBytes(const std::string& path, const std::string& bytes) {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(bytes.data(),
                      static_cast<std::streamsize>(bytes.size()));
            return static_cast<bool>(out);
        }

        // `cajeta add <name>[@<version>] [--manifest=<path>]`
        // Inserts (or replaces) the entry in settings.dependencies.
        // Default constraint when `@version` is omitted: "*".
        int addCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string spec;
            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = value;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta add <name>[@<version>] "
                        << "[--manifest=<path>]\n"
                        << "\n"
                        << "Add a dependency to settings.dependencies. "
                        << "Default constraint when version is omitted: *.\n";
                    return 0;
                } else if (!arg.empty() && arg[0] == '-') {
                    std::cerr << "cajeta add: unknown argument '"
                              << arg << "'\n";
                    return 1;
                } else if (spec.empty()) {
                    spec = std::string(arg);
                } else {
                    std::cerr << "cajeta add: unexpected positional "
                                 "argument '" << arg << "'\n";
                    return 1;
                }
            }
            if (spec.empty()) {
                std::cerr << "cajeta add: missing required dependency "
                             "name (use `cajeta add <name>[@<version>]`)\n";
                return 1;
            }

            // Split spec at the first '@'. Everything to the right
            // is the constraint; default to '*' when absent.
            std::string name = spec;
            std::string constraint = "*";
            auto at = spec.find('@');
            if (at != std::string::npos) {
                name = spec.substr(0, at);
                constraint = spec.substr(at + 1);
                if (constraint.empty()) constraint = "*";
            }
            if (name.empty()) {
                std::cerr << "cajeta add: dependency name is empty\n";
                return 1;
            }

            std::string src;
            if (!readFileBytes(manifestPath, src)) {
                std::cerr << "cajeta add: cannot read manifest '"
                          << manifestPath << "'\n";
                return 1;
            }
            auto out = addDependencyToManifest(src, name, constraint);
            if (!out) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << out.takeError();
                std::cerr << "cajeta add: " << msg << "\n";
                return 1;
            }
            if (!writeFileBytes(manifestPath, *out)) {
                std::cerr << "cajeta add: cannot write manifest '"
                          << manifestPath << "'\n";
                return 1;
            }
            std::cout << "added " << name << " (" << constraint
                      << ") to " << manifestPath << "\n";
            return 0;
        }

        // `cajeta remove <name> [--manifest=<path>]`
        // Removes the entry from settings.dependencies. Errors when
        // the dep isn't declared.
        int removeCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string name;
            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = value;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta remove <name> "
                        << "[--manifest=<path>]\n"
                        << "\n"
                        << "Remove a dependency from "
                        << "settings.dependencies.\n";
                    return 0;
                } else if (!arg.empty() && arg[0] == '-') {
                    std::cerr << "cajeta remove: unknown argument '"
                              << arg << "'\n";
                    return 1;
                } else if (name.empty()) {
                    name = std::string(arg);
                } else {
                    std::cerr << "cajeta remove: unexpected positional "
                                 "argument '" << arg << "'\n";
                    return 1;
                }
            }
            if (name.empty()) {
                std::cerr << "cajeta remove: missing required "
                             "dependency name\n";
                return 1;
            }
            std::string src;
            if (!readFileBytes(manifestPath, src)) {
                std::cerr << "cajeta remove: cannot read manifest '"
                          << manifestPath << "'\n";
                return 1;
            }
            auto out = removeDependencyFromManifest(src, name);
            if (!out) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << out.takeError();
                std::cerr << "cajeta remove: " << msg << "\n";
                return 1;
            }
            if (!writeFileBytes(manifestPath, *out)) {
                std::cerr << "cajeta remove: cannot write manifest '"
                          << manifestPath << "'\n";
                return 1;
            }
            std::cout << "removed " << name << " from " << manifestPath
                      << "\n";
            return 0;
        }

        // `cajeta upgrade [<name>[@<version>]]... [options]`
        //
        // Re-resolves each named dep (or every dep when no names
        // given) to the highest version in the configured repos and
        // rewrites its constraint in the manifest to an exact pin.
        // When the upgrade would add capabilities the consumer hasn't
        // declared, prompts y/N before writing.
        //
        // --dry-run         Print the plan, don't write.
        // --yes / -y        Skip the prompt (write even when caps grow).
        // --manifest=<p>    Manifest file (default: ./cajeta.json).
        int upgradeCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            bool dryRun = false;
            bool assumeYes = false;
            std::vector<std::string> names;
            std::map<std::string, std::string> explicitVersions;

            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = value;
                } else if (arg == "--dry-run") {
                    dryRun = true;
                } else if (arg == "--yes" || arg == "-y") {
                    assumeYes = true;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta upgrade [<name>[@<version>]...] "
                        << "[options]\n"
                        << "\n"
                        << "Upgrade declared dependencies to their "
                        << "highest available version (or to the "
                        << "explicit version when given). With no "
                        << "names, upgrades every direct dep.\n"
                        << "\n"
                        << "  --dry-run            Print the plan; "
                        << "don't write.\n"
                        << "  --yes / -y           Skip the prompt "
                        << "when capabilities change.\n"
                        << "  --manifest=<path>    Manifest file "
                        << "(default: ./cajeta.json).\n";
                    return 0;
                } else if (!arg.empty() && arg[0] == '-') {
                    std::cerr << "cajeta upgrade: unknown argument '"
                              << arg << "'\n";
                    return 1;
                } else {
                    // Positional: <name> or <name>@<version>
                    std::string spec(arg);
                    std::string n = spec;
                    auto at = spec.find('@');
                    if (at != std::string::npos) {
                        n = spec.substr(0, at);
                        std::string v = spec.substr(at + 1);
                        if (n.empty() || v.empty()) {
                            std::cerr << "cajeta upgrade: malformed "
                                         "spec '" << spec
                                      << "' (expected name@version)\n";
                            return 1;
                        }
                        explicitVersions[n] = v;
                    }
                    names.push_back(n);
                }
            }

            // Load + parse the manifest (also gives us the source
            // bytes for the rewrite step).
            auto manifest = loadManifestFile(manifestPath);
            if (!manifest) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << manifest.takeError();
                std::cerr << "cajeta upgrade: " << msg << "\n";
                return 1;
            }
            std::string manifestSrc;
            if (!readFileBytes(manifestPath, manifestSrc)) {
                std::cerr << "cajeta upgrade: cannot read manifest '"
                          << manifestPath << "'\n";
                return 1;
            }
            std::string projectRoot =
                std::filesystem::path(manifestPath)
                    .parent_path().string();
            if (projectRoot.empty()) projectRoot = ".";

            auto plan = planUpgrade(
                *manifest, projectRoot, names, explicitVersions);
            if (!plan) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << plan.takeError();
                std::cerr << "cajeta upgrade: " << msg << "\n";
                return 1;
            }

            // Print the plan.
            int changedCount = 0;
            for (const auto& e : plan->entries) {
                if (e.changed) {
                    ++changedCount;
                    std::cout << "  " << e.name << ": "
                              << (e.oldVersion.empty()
                                      ? e.oldConstraint
                                      : e.oldVersion)
                              << " -> " << e.newVersion
                              << " (from " << e.resolvedFromRepo << ")";
                    if (!e.capDelta.empty()) {
                        std::cout << "  [capabilities changed]";
                    }
                    std::cout << "\n";
                    if (!e.capDelta.added.empty()) {
                        std::cout << "      + caps:";
                        for (const auto& c : e.capDelta.added) {
                            std::cout << " " << c;
                        }
                        std::cout << "\n";
                    }
                    if (!e.capDelta.removed.empty()) {
                        std::cout << "      - caps:";
                        for (const auto& c : e.capDelta.removed) {
                            std::cout << " " << c;
                        }
                        std::cout << "\n";
                    }
                } else {
                    std::cout << "  " << e.name << ": already at "
                              << e.newVersion << " — no change\n";
                }
            }

            if (changedCount == 0) {
                std::cout << "nothing to upgrade.\n";
                return 0;
            }

            if (dryRun) {
                std::cout << "(dry-run; manifest not modified)\n";
                return 0;
            }

            // Capability-change prompt. Skip when --yes, or when
            // stdin isn't a TTY (caller is a script — we error out
            // with a hint instead of silently applying).
            if (plan->anyCapabilityChange() && !assumeYes) {
                bool isTty = isatty(STDIN_FILENO) != 0;
                if (!isTty) {
                    std::cerr << "cajeta upgrade: upgrade adds new "
                                 "capabilities; rerun with --yes to "
                                 "confirm in a non-interactive "
                                 "session.\n";
                    return 1;
                }
                std::cout << "\nApply these upgrades (new "
                             "capabilities will be granted)? [y/N] ";
                std::cout.flush();
                std::string answer;
                if (!std::getline(std::cin, answer) ||
                    (answer != "y" && answer != "Y" &&
                     answer != "yes" && answer != "YES")) {
                    std::cout << "aborted.\n";
                    return 1;
                }
            }

            auto rewritten = applyUpgradePlan(manifestSrc, *plan);
            if (!rewritten) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << rewritten.takeError();
                std::cerr << "cajeta upgrade: " << msg << "\n";
                return 1;
            }
            if (!writeFileBytes(manifestPath, *rewritten)) {
                std::cerr << "cajeta upgrade: cannot write manifest '"
                          << manifestPath << "'\n";
                return 1;
            }
            std::cout << "upgraded " << changedCount
                      << " dependency(ies) in " << manifestPath << "\n";
            return 0;
        }

    } // namespace

    namespace {

        // Load manifest + resolve properties + parse tasks. Common
        // prologue for the task-related subcommands.
        struct LoadedProject {
            Manifest manifest;
            ResolvedProperties props;
            std::map<std::string, Task> tasks;
        };

        llvm::Expected<LoadedProject> loadProject(
            const std::string& manifestPath,
            const PropertyOverrides& overrides) {
            auto manifest = loadManifestFile(manifestPath);
            if (!manifest) return manifest.takeError();
            auto props = resolveProperties(*manifest, overrides);
            if (!props) return props.takeError();
            auto tasks = parseTasks(*manifest);
            if (!tasks) return tasks.takeError();
            // Cycle / undefined-dep validation up front. Catches
            // structural errors before any task starts running.
            if (auto e = validateTaskGraph(*tasks)) return std::move(e);
            LoadedProject p;
            p.manifest = std::move(*manifest);
            p.props = std::move(*props);
            p.tasks = std::move(*tasks);
            return p;
        }

        // `cajeta task <name> --show` — print the resolved action
        // sequence for a task without running it.
        int taskShowCommand(int argc, const char* argv[]) {
            if (argc < 3) {
                std::cerr << "Usage: cajeta task <name> --show [--manifest=<path>]\n";
                return 1;
            }
            std::string taskName = argv[2];
            std::string manifestPath = "./cajeta.json";
            PropertyOverrides overrides;
            loadEnvOverrides(overrides);
            TaskInvocationParams cliParams;
            bool wantShow = false;

            for (int i = 3; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (arg == "--show") {
                    wantShow = true;
                } else if (match(arg, "manifest", value)) {
                    manifestPath = std::move(value);
                } else if (arg == "-p" && i + 1 < argc) {
                    auto parsed = parseCliOverride(argv[++i]);
                    if (!parsed) {
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << parsed.takeError();
                        std::cerr << "cajeta task " << taskName << ": "
                                  << msg << "\n";
                        return 1;
                    }
                    cliParams.values[parsed->first] = parsed->second;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout << "Usage: cajeta task <name> --show "
                                 "[--manifest=<path>] [-p NAME=VALUE]\n"
                              << "Print the resolved action sequence "
                                 "for a task without running it.\n";
                    return 0;
                } else {
                    std::cerr << "cajeta task " << taskName
                              << ": unknown argument '" << arg << "'\n";
                    return 1;
                }
            }

            if (!wantShow) {
                std::cerr << "cajeta task: --show is required (no other "
                             "task subcommands today)\n";
                return 1;
            }

            auto project = loadProject(manifestPath, overrides);
            if (!project) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << project.takeError();
                std::cerr << "cajeta task " << taskName << ": " << msg << "\n";
                return 1;
            }
            if (auto e = showTask(project->tasks, taskName, cliParams,
                                  project->props, std::cout,
                                  &project->manifest)) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << e;
                std::cerr << "cajeta task " << taskName << ": " << msg << "\n";
                return 1;
            }
            return 0;
        }

        // `cajeta tasks` — list task names + descriptions.
        int tasksCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            PropertyOverrides overrides;
            loadEnvOverrides(overrides);

            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = std::move(value);
                } else if (arg == "--help" || arg == "-h") {
                    std::cout << "Usage: cajeta tasks [--manifest=<path>]\n"
                              << "List tasks defined in the manifest.\n";
                    return 0;
                } else {
                    std::cerr << "cajeta tasks: unknown argument '"
                              << arg << "'\n";
                    return 1;
                }
            }

            auto project = loadProject(manifestPath, overrides);
            if (!project) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << project.takeError();
                std::cerr << "cajeta tasks: " << msg << "\n";
                return 1;
            }
            if (project->tasks.empty()) {
                std::cout << "(no tasks defined in " << manifestPath << ")\n";
                return 0;
            }
            // Compute column width for nice alignment.
            size_t nameWidth = 0;
            for (const auto& kv : project->tasks) {
                if (kv.first.size() > nameWidth) nameWidth = kv.first.size();
            }
            for (const auto& kv : project->tasks) {
                std::cout << "  " << kv.first;
                for (size_t i = kv.first.size(); i < nameWidth + 2; ++i) {
                    std::cout << ' ';
                }
                if (kv.second.description) {
                    std::cout << *kv.second.description;
                }
                std::cout << "\n";
            }
            return 0;
        }

        // Dispatch a named task. Parses CLI args into property /
        // task-param overrides, then invokes the runner.
        int runTaskCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string taskName = argv[1];
            PropertyOverrides overrides;
            loadEnvOverrides(overrides);
            TaskInvocationParams cliParams;

            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = std::move(value);
                } else if (arg == "-P" && i + 1 < argc) {
                    auto parsed = parseCliOverride(argv[++i]);
                    if (!parsed) {
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << parsed.takeError();
                        std::cerr << "cajeta " << taskName << ": " << msg << "\n";
                        return 1;
                    }
                    overrides.cli[parsed->first] = parsed->second;
                } else if (match(arg, "property", value)) {
                    auto parsed = parseCliOverride(value);
                    if (!parsed) {
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << parsed.takeError();
                        std::cerr << "cajeta " << taskName << ": " << msg << "\n";
                        return 1;
                    }
                    overrides.cli[parsed->first] = parsed->second;
                } else if (arg == "-p" && i + 1 < argc) {
                    auto parsed = parseCliOverride(argv[++i]);
                    if (!parsed) {
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << parsed.takeError();
                        std::cerr << "cajeta " << taskName << ": " << msg << "\n";
                        return 1;
                    }
                    cliParams.values[parsed->first] = parsed->second;
                } else if (match(arg, "param", value)) {
                    auto parsed = parseCliOverride(value);
                    if (!parsed) {
                        std::string msg;
                        llvm::raw_string_ostream os(msg);
                        os << parsed.takeError();
                        std::cerr << "cajeta " << taskName << ": " << msg << "\n";
                        return 1;
                    }
                    cliParams.values[parsed->first] = parsed->second;
                } else if (match(arg, "flavor", value)) {
                    overrides.flavor = value;
                } else if (match(arg, "profile", value)) {
                    overrides.profile = value;
                } else {
                    std::cerr << "cajeta " << taskName
                              << ": unknown argument '" << arg << "'\n";
                    return 1;
                }
            }

            auto project = loadProject(manifestPath, overrides);
            if (!project) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << project.takeError();
                std::cerr << "cajeta " << taskName << ": " << msg << "\n";
                return 1;
            }

            ActionRegistry registry;
            auto outputs = runTask(
                project->tasks, taskName, cliParams,
                project->props, registry, &project->manifest);
            if (!outputs) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << outputs.takeError();
                std::cerr << "cajeta " << taskName << ": " << msg << "\n";
                return 1;
            }

            // Print the task's outputs (when non-empty). Useful for
            // scripting; quiet by default for tasks that have no
            // declared outputs.
            if (!outputs->empty()) {
                std::cout << "\nTask '" << taskName << "' outputs:\n";
                for (const auto& kv : *outputs) {
                    std::cout << "  " << kv.first << " = " << kv.second << "\n";
                }
            }
            return 0;
        }

        // Decide whether `argv[1]` is a task invocation. Returns
        // false (so the compiler-side fallthrough runs) when there's
        // no manifest or no matching task — that way `cajeta archive`
        // etc. still work outside a project.
        bool looksLikeTaskInvocation(int argc, const char* argv[]) {
            if (argc < 2) return false;
            std::string_view cmd = argv[1];
            // Built-in subcommands handled elsewhere.
            if (cmd == "info" || cmd == "tasks" || cmd == "task" ||
                cmd == "init" || cmd == "archive" ||
                cmd == "add"  || cmd == "remove" ||
                cmd == "upgrade") {
                return false;
            }
            // Anything starting with `-` is a flag for the existing
            // compiler invocation, not a task name.
            if (!cmd.empty() && cmd[0] == '-') return false;
            // Look for a manifest in the current directory; only
            // claim to handle the task if we find one.
            llvm::Expected<llvm::json::Value> probe =
                parseJsonCFile("./cajeta.json");
            if (!probe) {
                llvm::consumeError(probe.takeError());
                return false;
            }
            const auto* root = probe->getAsObject();
            if (!root) return false;
            const auto* tasksBlock = root->getObject("tasks");
            if (!tasksBlock) return false;
            return tasksBlock->get(std::string(cmd)) != nullptr;
        }

    } // namespace

    bool dispatchBuildTool(int argc, const char* argv[], int* exitCodeOut) {
        if (argc < 2) return false;
        std::string_view cmd = argv[1];

        if (cmd == "info") {
            *exitCodeOut = infoCommand(argc, argv);
            return true;
        }
        if (cmd == "tasks") {
            *exitCodeOut = tasksCommand(argc, argv);
            return true;
        }
        if (cmd == "task") {
            *exitCodeOut = taskShowCommand(argc, argv);
            return true;
        }
        if (cmd == "init") {
            *exitCodeOut = initCommand(argc, argv);
            return true;
        }
        if (cmd == "add") {
            *exitCodeOut = addCommand(argc, argv);
            return true;
        }
        if (cmd == "remove") {
            *exitCodeOut = removeCommand(argc, argv);
            return true;
        }
        if (cmd == "upgrade") {
            *exitCodeOut = upgradeCommand(argc, argv);
            return true;
        }
        if (looksLikeTaskInvocation(argc, argv)) {
            *exitCodeOut = runTaskCommand(argc, argv);
            return true;
        }

        // Other build-tool subcommands land in subsequent phases. For
        // now anything not recognized falls through to the compiler.
        return false;
    }

} // namespace cajeta::buildtool
