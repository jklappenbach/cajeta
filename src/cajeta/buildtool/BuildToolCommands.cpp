#include "cajeta/buildtool/BuildToolCommands.h"

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/ArtifactCache.h"
#include "cajeta/buildtool/DiagnosticFormat.h"
#include "cajeta/buildtool/InitTemplates.h"
#include "cajeta/buildtool/JsonC.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Plugin.h"
#include "cajeta/buildtool/PluginAction.h"
#include "cajeta/buildtool/Repository.h"
#include "cajeta/buildtool/ManifestEditor.h"
#include "cajeta/buildtool/Melt.h"
#include "cajeta/buildtool/OllaStore.h"
#include "cajeta/buildtool/repo/FilesystemRepository.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Provenance.h"
#include "cajeta/buildtool/Reproducibility.h"
#include "cajeta/buildtool/Resolver.h"
#include "cajeta/buildtool/Sandbox.h"
#include "cajeta/buildtool/Toolchain.h"
#include "cajeta/buildtool/Task.h"
#include "cajeta/buildtool/TaskRunner.h"
#include "cajeta/buildtool/Upgrader.h"
#include "cajeta/buildtool/Workspace.h"
#include "cajeta/buildtool/skill/SkillCli.h"
#include "cajeta/buildtool/skill/SkillGet.h"
#include "cajeta/dap/Json.h"
#include "cajeta/cli/SignatureVerify.h"
#include "cajeta/cli/TrustStore.h"
#include "cajeta/kernel/KernelMain.h"

#include "llvm/Support/FileSystem.h"

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

        // Apply a `--diag-format=text|json` value to the process-wide diagnostic
        // format (json-diagnostics-spec §2). Returns false on an invalid value.
        bool applyDiagFormatArg(const std::string& value) {
            if (value == "json") { setDiagnosticFormat(DiagFormat::Json); return true; }
            if (value == "text") { setDiagnosticFormat(DiagFormat::Text); return true; }
            return false;
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
        // at runtime). See docs/BuildTool.md "Project shapes".
        int initCommand(int argc, const char* argv[]) {
            std::string templateName = "basic";
            std::string destDir = ".";
            bool force = false;
            bool listOnly = false;

            // `cajeta init --kernel` installs the Jupyter kernelspec rather
            // than scaffolding a project (jupyter-kernel spec §3). It shares
            // the verb because it is the same act — putting something on disk
            // so a tool can find it — and it is checked first because it
            // takes no archetype and no destination.
            for (int i = 2; i < argc; ++i) {
                if (std::string_view(argv[i]) != "--kernel") continue;
                bool replace = false;
                for (int j = 2; j < argc; ++j) {
                    std::string_view a = argv[j];
                    if (a == "--force" || a == "-f") replace = true;
                }
                std::string exe = llvm::sys::fs::getMainExecutable(
                    argv[0], reinterpret_cast<void*>(&initCommand));
                if (exe.empty()) exe = "cajeta";
                std::string error;
                std::string written =
                    cajeta::kernel::installKernelSpec(exe, replace, &error);
                if (written.empty()) {
                    std::cerr << "cajeta init --kernel: " << error << "\n";
                    return 1;
                }
                std::cout << "Installed the cajeta kernelspec:\n  " << written
                          << "\nStart a notebook with `jupyter lab` and pick "
                             "the Cajeta kernel.\n";
                return 0;
            }

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

            // 7.2.11 — a notebook project is unusable until Jupyter can find
            // the kernel, so init is where the kernelspec lands. An already-
            // installed spec is success, not an error; `cajeta task
            // kernelspec` force-refreshes after a toolchain upgrade.
            if (templateName == "notebook") {
                std::string exe = llvm::sys::fs::getMainExecutable(
                    argv[0], reinterpret_cast<void*>(&initCommand));
                if (exe.empty()) exe = "cajeta";
                std::string specError;
                std::string written = cajeta::kernel::installKernelSpec(
                    exe, /*force=*/false, &specError);
                if (!written.empty()) {
                    std::cout << "Installed the cajeta kernelspec:\n  "
                              << written << "\n";
                } else if (specError.find("already installed")
                               != std::string::npos) {
                    std::cout << "Jupyter kernelspec already installed.\n";
                } else {
                    std::cout << "warning: kernelspec not installed: "
                              << specError << "\n";
                }
                std::cout
                    << "\nNext steps:\n"
                    << "  1. Edit cajeta.json's `details.name`; add "
                       "dependencies with `cajeta add`.\n"
                    << "  2. cajeta run         # open Jupyter Lab on "
                       "notebooks/welcome.ipynb\n";
                return 0;
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
            bool meltMode = false;
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
                } else if (arg == "--melt") {
                    meltMode = true;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta upgrade [<name>[@<version>]...] "
                        << "[options]\n"
                        << "       cajeta upgrade --melt [<name>[@<version>]...] "
                        << "[options]\n"
                        << "\n"
                        << "Upgrade declared dependencies (or melts, with "
                        << "--melt) to their highest available version "
                        << "(or to the explicit version when given). "
                        << "With no names, upgrades every direct dep "
                        << "/ every imported melt.\n"
                        << "\n"
                        << "  --melt               Upgrade entries in "
                        << "settings.melts instead of settings.dependencies.\n"
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

            if (meltMode) {
                auto plan = planMeltUpgrade(
                    *manifest, projectRoot, names, explicitVersions);
                if (!plan) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << plan.takeError();
                    std::cerr << "cajeta upgrade: " << msg << "\n";
                    return 1;
                }
                int changedCount = 0;
                for (const auto& e : plan->entries) {
                    if (e.changed) {
                        ++changedCount;
                        std::cout << "  melt " << e.name << ": "
                                  << e.oldVersion << " -> "
                                  << e.newVersion << " (from "
                                  << e.resolvedFromRepo << ")";
                        if (!e.depDelta.empty()) {
                            std::cout << "  [curated deps changed]";
                        }
                        std::cout << "\n";
                        for (const auto& [n, c] : e.depDelta.added) {
                            std::cout << "      + " << n
                                      << " (" << c << ")\n";
                        }
                        for (const auto& [n, oldC, newC] :
                                 e.depDelta.changed) {
                            std::cout << "      ~ " << n << " "
                                      << oldC << " -> " << newC << "\n";
                        }
                        for (const auto& n : e.depDelta.removed) {
                            std::cout << "      - " << n << "\n";
                        }
                    } else {
                        std::cout << "  melt " << e.name
                                  << ": already at " << e.newVersion
                                  << " — no change\n";
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
                auto rewritten = applyMeltUpgradePlan(manifestSrc, *plan);
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
                          << " melt(s) in " << manifestPath << "\n";
                return 0;
            }

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
            // Rewrite ${...} in `settings` and `plugins` before anything
            // reads them. parseDependencies / parseRepositories /
            // parsePlugins all consume the raw JSON, so a placeholder that
            // survives to here reaches the resolver as if it were a version
            // constraint, or reaches a plugin as if it were a path.
            if (auto e = substituteManifestProperties(*manifest, *props)) {
                return std::move(e);
            }
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

        // The tool's built-in subcommands, surfaced in `tasks --json` so the IDE
        // can present them as runnable separately from manifest tasks (spec
        // §3.1.2). Curated runnable set; not every internal subcommand.
        std::vector<BuiltinCommand> builtinCommands() {
            return {
                {"init", "Scaffold a new project from an archetype"},
                {"add", "Add a dependency to the manifest"},
                {"remove", "Remove a dependency from the manifest"},
                {"info", "Print dependency tree / capabilities"},
                {"tasks", "List tasks defined in the manifest"},
                {"upgrade", "Upgrade dependencies to newer versions"},
                {"install", "Resolve and install dependencies"},
                {"publish", "Publish the project to a repository"},
                {"workspace", "Workspace (multi-member) operations"},
                {"toolchain", "Manage the active toolchain"},
                {"coverage", "Generate a coverage report"},
            };
        }

        // `cajeta tasks` — list task names + descriptions.
        int tasksCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            bool jsonOut = false;
            PropertyOverrides overrides;
            loadEnvOverrides(overrides);

            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = std::move(value);
                } else if (arg == "--json") {
                    jsonOut = true;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout << "Usage: cajeta tasks [--manifest=<path>] [--json]\n"
                              << "List tasks defined in the manifest.\n"
                              << "  --json  Emit the machine-readable task/builtins "
                                 "document (IDE contract, spec §3).\n";
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

            if (jsonOut) {
                // Absolute manifest path so the IDE can key linked roots on it.
                std::error_code ec;
                std::filesystem::path abs =
                    std::filesystem::absolute(manifestPath, ec);
                std::string absStr = ec ? manifestPath : abs.string();

                // Debug-launch coordinates (widget §5.2.2, unit 7): `cajeta dap`
                // JIT-runs an entry method from a source root, so surface those
                // from settings.build for the IDE's Debug executor. A parse miss
                // here just omits the coords (Debug disabled), never fails
                // discovery.
                DebugLaunchCoords debugCoords;
                if (auto sb = parseSettingsBuild(project->manifest)) {
                    debugCoords.sourceRoot = sb->sourceRoot;
                    debugCoords.entryMethod = sb->entryMethod;
                } else {
                    llvm::consumeError(sb.takeError());
                }

                std::cout << renderTasksJson(absStr, project->tasks,
                                             builtinCommands(), debugCoords)
                          << "\n";
                return 0;
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
        // Phase 12: resolve a possibly-`<member>:<task>` invocation
        // to the manifest path the task lives in, plus the bare task
        // name to look up. Returns std::nullopt for plain task names
        // (the caller falls back to the local `./cajeta.json`).
        //
        // The colon form requires a workspace root on the ancestor
        // chain. The member is looked up by short name; an unknown
        // member produces a structured error.
        struct ResolvedTaskRef {
            std::string manifestPath;
            std::string taskName;
        };
        llvm::Expected<std::optional<ResolvedTaskRef>>
        resolveCrossMemberRef(std::string_view raw) {
            auto colon = raw.find(':');
            if (colon == std::string_view::npos) return std::optional<ResolvedTaskRef>{};
            // Bare leading colon, trailing colon, or empty halves
            // are user typos — surface them rather than silently
            // treating as a single-segment name.
            std::string memberName{raw.substr(0, colon)};
            std::string taskName{raw.substr(colon + 1)};
            if (memberName.empty() || taskName.empty()) {
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(),
                    "cross-member task '" + std::string(raw) +
                    "' must be of the form '<member>:<task>'");
            }
            auto wsRoot = discoverWorkspaceRoot(".");
            if (!wsRoot) {
                return llvm::createStringError(
                    llvm::inconvertibleErrorCode(),
                    "'" + memberName + ":" + taskName +
                    "' requires a workspace ancestor with a "
                    "'workspace' block; none found from cwd");
            }
            auto ws = loadWorkspace(*wsRoot);
            if (!ws) return ws.takeError();
            for (const auto& m : ws->members) {
                if (memberShortName(m) == memberName) {
                    return std::optional<ResolvedTaskRef>{
                        ResolvedTaskRef{m.manifestPath, taskName}};
                }
            }
            std::string known;
            for (const auto& m : ws->members) {
                if (!known.empty()) known += ", ";
                known += memberShortName(m);
            }
            return llvm::createStringError(
                llvm::inconvertibleErrorCode(),
                "no workspace member named '" + memberName +
                "' (known: " + known + ")");
        }
        // Join the plugin model to the task path: resolve the manifest's
        // `plugins` block and register each plugin-provided action in the
        // registry, so tasks can name them exactly like builtins. Every
        // component here — parsePlugins, resolvePlugins, PluginRuntime,
        // PluginAction — predates this function; the missing piece was
        // only this wiring, which is why a task naming a plugin action
        // has always failed with "unknown action".
        llvm::Error wireManifestPlugins(const Manifest& m,
                                        const std::string& manifestPath,
                                        ActionRegistry& registry) {
            auto specs = parsePlugins(m);
            if (!specs) return specs.takeError();
            if (specs->empty()) return llvm::Error::success();

            std::string projectRoot = std::filesystem::path(manifestPath)
                                          .parent_path()
                                          .string();
            if (projectRoot.empty()) projectRoot = ".";

            auto repoSpecs = parseRepositories(m);
            if (!repoSpecs) return repoSpecs.takeError();
            std::string downloadStage =
                (std::filesystem::path(projectRoot) / ".cajeta" / "cache" /
                 "downloads")
                    .string();
            auto repos = buildRepositories(*repoSpecs, downloadStage);
            if (!repos) return repos.takeError();

            // Local-first, exactly as dependency resolution does it
            // (Resolver.cpp): prepend the implicit ~/.olla store as the
            // highest-priority source. Without this, `cajeta install` of a
            // plugin produced an artifact nothing could consume — the store
            // held 0.4.0 and resolution still reported "no repository has a
            // satisfying version (tried: central)", so the only way to test a
            // plugin change was to publish it to a remote registry first.
            repos->insert(repos->begin(),
                std::make_shared<FilesystemRepository>(
                    "olla", OllaStore::resolveRoot()));

            auto allowed = parsePluginsAllowedCapabilities(m);
            if (!allowed) return allowed.takeError();
            ArtifactCache cache(projectRoot);
            auto resolved = resolvePlugins(*specs, *repos, *allowed, cache);
            if (!resolved) return resolved.takeError();

            for (const auto& rp : *resolved) {
                // The spec's config block is each action's default param
                // layer (the contract Plugin.h documents).
                llvm::json::Object config;
                for (const auto& sp : *specs) {
                    if (sp.name == rp.name) {
                        config = sp.configRaw;
                        break;
                    }
                }
                for (auto& a : makePluginActions(rp, config)) {
                    registry.registerAction(std::move(a));
                }
            }
            return llvm::Error::success();
        }



        int runTaskCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string taskName = argv[1];
            // Phase 12: `<member>:<task>` reroutes the task lookup
            // to a sibling member's manifest before any property
            // resolution runs. The bare task name is restored so
            // the rest of the dispatch is identical to a normal
            // run-task invocation.
            {
                auto ref = resolveCrossMemberRef(taskName);
                if (!ref) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << ref.takeError();
                    std::cerr << "cajeta " << taskName << ": "
                              << msg << "\n";
                    return 1;
                }
                if (ref->has_value()) {
                    manifestPath = (**ref).manifestPath;
                    taskName = (**ref).taskName;
                }
            }
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
                } else if (match(arg, "diag-format", value)) {
                    if (!applyDiagFormatArg(value)) {
                        std::cerr << "cajeta " << taskName
                                  << ": --diag-format must be text|json\n";
                        return 1;
                    }
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
            if (auto e = wireManifestPlugins(project->manifest, manifestPath,
                                             registry)) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << std::move(e);
                std::cerr << "cajeta " << taskName << ": " << msg << "\n";
                return 1;
            }
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

        // `cajeta coverage {ignore,list,remove}` — manipulate the
        // cajeta.coverage plugin's exclude list in cajeta.json.
        //
        // The IDE plugin's "right-click → ignore in coverage" action
        // shells out to `cajeta coverage ignore --kind <k> --pattern
        // <p> --reason <r>`; CI scripts can call `cajeta coverage
        // list` to enumerate the current exclusions before opening a
        // PR; `cajeta coverage remove` deletes an entry by pattern.
        //
        // The JSONC-preserving rewrite lives in ManifestEditor —
        // this layer is argv-parsing + a small generic-reason filter
        // that catches obvious sloppy reasons before they hit disk.

        // Reasons we refuse to record. Lowercase, exact-match; the
        // plugin author's own config parser may extend this list,
        // but the CLI catches the most common drift before it hits
        // disk. See plans/buildtool/coverage-exclude-and-cli.md "Generic-reason
        // list" for the rationale.

        bool isGenericCoverageReason(std::string_view reason) {
            std::string lower;
            lower.reserve(reason.size());
            for (char c : reason) {
                lower += static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            }
            // Trim surrounding whitespace.
            size_t b = 0, e = lower.size();
            while (b < e && std::isspace(
                       static_cast<unsigned char>(lower[b]))) ++b;
            while (e > b && std::isspace(
                       static_cast<unsigned char>(lower[e - 1]))) --e;
            lower = lower.substr(b, e - b);
            return lower.empty() ||
                   lower == "wip"   || lower == "todo"  ||
                   lower == "skip"  || lower == "fixme" ||
                   lower == "tbd";
        }

        // Pull the exclude array out of the manifest. Returns an
        // empty vector when the plugin isn't declared (callers print
        // the "no entries" message). Each entry is the JSON value as
        // parsed — typed objects or back-compat strings.
        // Plugin ids that mean coverage, newest first.
        //
        // `dev.cajeta.coverage` is what the resolver publishes and what real
        // manifests declare; `cajeta.coverage` is the first-party name
        // BuildTool.md documented for a 1.0 that was never built. Matching only
        // the latter made `cajeta coverage list` report "no exclude entries
        // declared" for a project that had two — the same id mismatch that made
        // the IDE read a configured project as not using coverage at all.
        const char* const kCoveragePluginIds[] = {
            "dev.cajeta.coverage", "cajeta.coverage"
        };

        const llvm::json::Object*
        findCoveragePlugin(const llvm::json::Object& root) {
            const auto* plugins = root.getObject("plugins");
            if (!plugins) return nullptr;
            for (const char* id : kCoveragePluginIds) {
                if (const auto* cov = plugins->getObject(id)) return cov;
            }
            return nullptr;
        }

        std::vector<llvm::json::Value>
        readCoverageExcludes(const llvm::json::Object& root) {
            std::vector<llvm::json::Value> out;
            const auto* cov = findCoveragePlugin(root);
            if (!cov) return out;
            const auto* config = cov->getObject("config");
            if (!config) return out;
            const auto* arr = config->getArray("exclude");
            if (!arr) return out;
            out.reserve(arr->size());
            for (const auto& v : *arr) {
                out.push_back(v);
            }
            return out;
        }

        int coverageIgnoreCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string kind;
            std::string pattern;
            std::string reason;

            for (int i = 3; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = value;
                } else if (match(arg, "kind",    value)) {
                    kind = value;
                } else if (match(arg, "pattern", value)) {
                    pattern = value;
                } else if (match(arg, "reason",  value)) {
                    reason = value;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta coverage ignore "
                        << "--kind=<file|package|symbol> "
                        << "--pattern=<glob> --reason=<text> "
                        << "[--manifest=<path>]\n"
                        << "\n"
                        << "Add a typed exclude entry to "
                        << "plugins.cajeta.coverage.config.exclude.\n"
                        << "Refuses generic reasons "
                        << "(wip/todo/skip/fixme/tbd) and duplicates.\n";
                    return 0;
                } else {
                    std::cerr << "cajeta coverage ignore: unknown "
                                 "argument '" << arg << "'\n";
                    return 1;
                }
            }
            if (kind.empty() || pattern.empty() || reason.empty()) {
                std::cerr << "cajeta coverage ignore: --kind, "
                             "--pattern, and --reason are all "
                             "required\n";
                return 1;
            }
            if (isGenericCoverageReason(reason)) {
                std::cerr << "cajeta coverage ignore: '" << reason
                          << "' is too generic; provide a specific "
                          << "justification (the reason shows up on "
                          << "every PR diff)\n";
                return 1;
            }

            std::string src;
            if (!readFileBytes(manifestPath, src)) {
                std::cerr << "cajeta coverage ignore: cannot read "
                          << "manifest '" << manifestPath << "'\n";
                return 1;
            }
            auto out = appendCoverageExclude(src, kind, pattern, reason);
            if (!out) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << out.takeError();
                std::cerr << "cajeta coverage ignore: " << msg << "\n";
                return 1;
            }
            if (!writeFileBytes(manifestPath, *out)) {
                std::cerr << "cajeta coverage ignore: cannot write "
                          << "manifest '" << manifestPath << "'\n";
                return 1;
            }
            std::cout << "added " << kind << " exclude '" << pattern
                      << "' to " << manifestPath << "\n";
            return 0;
        }

        int coverageListCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string kindFilter;

            for (int i = 3; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = value;
                } else if (match(arg, "kind", value)) {
                    kindFilter = value;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta coverage list "
                        << "[--kind=<file|package|symbol>] "
                        << "[--manifest=<path>]\n"
                        << "\n"
                        << "Print the cajeta.coverage exclude list.\n";
                    return 0;
                } else {
                    std::cerr << "cajeta coverage list: unknown "
                                 "argument '" << arg << "'\n";
                    return 1;
                }
            }

            std::string src;
            if (!readFileBytes(manifestPath, src)) {
                std::cerr << "cajeta coverage list: cannot read "
                          << "manifest '" << manifestPath << "'\n";
                return 1;
            }
            auto parsed = parseJsonC(src);
            if (!parsed) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << parsed.takeError();
                std::cerr << "cajeta coverage list: " << msg << "\n";
                return 1;
            }
            const auto* root = parsed->getAsObject();
            if (!root) {
                std::cerr << "cajeta coverage list: manifest root is "
                             "not an object\n";
                return 1;
            }
            auto excludes = readCoverageExcludes(*root);
            if (excludes.empty()) {
                std::cout << "no exclude entries declared "
                             "(plugins.cajeta.coverage.config.exclude)\n";
                return 0;
            }
            int shown = 0;
            for (const auto& v : excludes) {
                std::string kind, pattern, reason;
                if (const auto* obj = v.getAsObject()) {
                    if (auto s = obj->getString("kind"))
                        kind = s->str();
                    if (auto s = obj->getString("pattern"))
                        pattern = s->str();
                    if (auto s = obj->getString("reason"))
                        reason = s->str();
                } else if (auto s = v.getAsString()) {
                    // Back-compat: bare string entry → implicit file kind.
                    kind = "file";
                    pattern = s->str();
                    reason = "(legacy entry; no reason recorded)";
                }
                if (!kindFilter.empty() && kind != kindFilter) continue;
                std::cout << "  " << kind << "\t" << pattern;
                if (!reason.empty()) {
                    std::cout << "\t— " << reason;
                }
                std::cout << "\n";
                ++shown;
            }
            if (shown == 0) {
                std::cout << "no entries matched kind '" << kindFilter
                          << "'\n";
            }
            return 0;
        }

        int coverageRemoveCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string pattern;
            bool assumeYes = false;

            for (int i = 3; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (match(arg, "manifest", value)) {
                    manifestPath = value;
                } else if (match(arg, "pattern", value)) {
                    pattern = value;
                } else if (arg == "--yes" || arg == "-y") {
                    assumeYes = true;
                } else if (arg == "--help" || arg == "-h") {
                    std::cout
                        << "Usage: cajeta coverage remove "
                        << "--pattern=<glob> "
                        << "[--manifest=<path>] [--yes]\n"
                        << "\n"
                        << "Remove every exclude entry matching the "
                        << "given pattern. Prompts on a TTY unless "
                        << "--yes is passed.\n";
                    return 0;
                } else {
                    std::cerr << "cajeta coverage remove: unknown "
                                 "argument '" << arg << "'\n";
                    return 1;
                }
            }
            if (pattern.empty()) {
                std::cerr << "cajeta coverage remove: --pattern is "
                             "required\n";
                return 1;
            }

            std::string src;
            if (!readFileBytes(manifestPath, src)) {
                std::cerr << "cajeta coverage remove: cannot read "
                          << "manifest '" << manifestPath << "'\n";
                return 1;
            }
            // Confirmation prompt when interactive. Removing an
            // entry is irreversible (well, modulo re-running ignore
            // with the same reason); skipping the prompt with --yes
            // makes CI scripts straightforward.
            if (!assumeYes && isatty(STDIN_FILENO) != 0) {
                std::cout << "Remove cajeta.coverage exclude "
                          << "matching '" << pattern << "' from "
                          << manifestPath << "? [y/N] ";
                std::string line;
                std::getline(std::cin, line);
                if (line.empty() ||
                    (line[0] != 'y' && line[0] != 'Y')) {
                    std::cout << "aborted.\n";
                    return 1;
                }
            }
            auto out = removeCoverageExclude(src, pattern);
            if (!out) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << out.takeError();
                std::cerr << "cajeta coverage remove: " << msg << "\n";
                return 1;
            }
            if (!writeFileBytes(manifestPath, out->newSource)) {
                std::cerr << "cajeta coverage remove: cannot write "
                          << "manifest '" << manifestPath << "'\n";
                return 1;
            }
            std::cout << "removed " << out->count
                      << " exclude(s) matching '" << pattern
                      << "' from " << manifestPath << "\n";
            return 0;
        }

        // `cajeta publish` — sugar over the publish action. Walks the
        // manifest for name + version, picks the archived-ir output
        // path from settings.build, accepts --url / --auth / --
        // signature / --archive overrides, and invokes the publish
        // action through the registry so the action's retry + transport
        // logic stays single-source.
        int publishCommand(int argc, const char* argv[]) {
            std::string manifestPath = "./cajeta.json";
            std::string urlArg;
            std::string archiveArg;
            std::string signatureArg;
            std::string keyIdArg;
            std::string authArg;
            for (int i = 2; i < argc; ++i) {
                std::string_view a = argv[i];
                std::string v;
                if (match(a, "manifest", v))      manifestPath = std::move(v);
                else if (match(a, "url", v))      urlArg       = std::move(v);
                else if (match(a, "archive", v))  archiveArg   = std::move(v);
                else if (match(a, "signature", v))signatureArg = std::move(v);
                else if (match(a, "key-id", v))   keyIdArg     = std::move(v);
                else if (match(a, "auth", v))     authArg      = std::move(v);
                else if (a == "--help" || a == "-h") {
                    std::cout
                        << "Usage: cajeta publish [options]\n"
                        << "\n"
                        << "  --manifest=PATH    cajeta.json (default ./cajeta.json)\n"
                        << "  --url=URL          registry URL (required)\n"
                        << "  --archive=PATH     .cja to publish (default settings.build output)\n"
                        << "  --signature=PATH   detached .sig\n"
                        << "  --key-id=ID        opaque signing key id\n"
                        << "  --auth=HEADER      Authorization header (e.g. 'Bearer T')\n";
                    return 0;
                }
            }
            if (urlArg.empty()) {
                std::cerr << "cajeta publish: --url is required\n";
                return 2;
            }

            auto m = loadManifestFile(manifestPath);
            if (!m) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << m.takeError();
                std::cerr << "cajeta publish: " << msg << "\n";
                return 1;
            }

            // Default archive path mirrors BuildAction's archived-ir
            // emit shape: build/archive/<name>-<version>.cja.
            std::string archive = archiveArg;
            if (archive.empty()) {
                auto sb = parseSettingsBuild(*m);
                if (!sb) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << sb.takeError();
                    std::cerr << "cajeta publish: " << msg << "\n";
                    return 1;
                }
                std::string outDir = sb->outputDir.value_or("build");
                archive = outDir + "/archive/" + m->details.name + "-" +
                          m->details.version + ".cja";
            }

            ResolvedProperties props;
            TaskContext ctx(props, &(*m));
            ActionRegistry reg;
            const Action* act = reg.get("publish");
            if (!act) {
                std::cerr << "cajeta publish: 'publish' action not registered\n";
                return 1;
            }

            llvm::json::Object params{
                {"archive", archive},
                {"url",     urlArg},
                {"name",    m->details.name},
                {"version", m->details.version},
            };
            if (!signatureArg.empty()) params["signature"] = signatureArg;
            if (!keyIdArg.empty())     params["key-id"]    = keyIdArg;
            if (!authArg.empty())      params["auth"]      = authArg;

            auto r = act->run(params, ctx);
            if (!r) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << r.takeError();
                std::cerr << "cajeta publish: " << msg << "\n";
                return 1;
            }
            for (const auto& kv : r->outputs) {
                std::cout << kv.first << "=" << kv.second << "\n";
            }
            return 0;
        }

        // ─── Phase 10 — `cajeta trust` subcommands ──────────────
        //
        // The trust store is the launcher's allow-list of ed25519
        // public keys. Subcommands live next to `cajeta publish` so
        // operators can keep one mental model: project tools work
        // on the manifest; trust tools work on `~/.cajeta/trust/`.

        int trustListCommand(int /*argc*/, const char* /*argv*/[]) {
            auto layout = cajeta::cli::resolveTrustStoreLayout();
            auto keys = cajeta::cli::listTrustedKeys(layout);
            if (keys.empty()) {
                std::cout << "(no trusted keys)\n";
                return 0;
            }
            for (const auto& k : keys) {
                std::cout << k.keyId
                          << "\t" << k.tier
                          << "\tsha256:" << k.fingerprint
                          << "\t" << k.path
                          << "\n";
            }
            return 0;
        }

        int trustAddCommand(int argc, const char* argv[]) {
            if (argc < 4) {
                std::cerr << "Usage: cajeta trust add <key-id> <pem-path>\n";
                return 2;
            }
            std::string keyId = argv[2];
            std::string pem = argv[3];
            auto layout = cajeta::cli::resolveTrustStoreLayout();
            if (auto e = cajeta::cli::addTrustedKey(layout, keyId, pem)) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << e;
                consumeError(std::move(e));
                std::cerr << msg << "\n";
                return 1;
            }
            std::cout << "added '" << keyId << "' to user trust store\n";
            return 0;
        }

        int trustRemoveCommand(int argc, const char* argv[]) {
            if (argc < 3) {
                std::cerr << "Usage: cajeta trust remove <key-id>\n";
                return 2;
            }
            std::string keyId = argv[2];
            auto layout = cajeta::cli::resolveTrustStoreLayout();
            if (auto e = cajeta::cli::removeTrustedKey(layout, keyId)) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << e;
                consumeError(std::move(e));
                std::cerr << msg << "\n";
                return 1;
            }
            std::cout << "removed '" << keyId << "' from user store\n";
            return 0;
        }

        int trustShowCommand(int argc, const char* argv[]) {
            if (argc < 3) {
                std::cerr << "Usage: cajeta trust show <key-id>\n";
                return 2;
            }
            std::string keyId = argv[2];
            auto layout = cajeta::cli::resolveTrustStoreLayout();
            auto e = cajeta::cli::lookupTrustedKey(layout, keyId);
            if (!e) {
                std::cerr << "trust show: '" << keyId
                          << "' not found in any tier\n";
                return 1;
            }
            std::cout << "key-id:      " << e->keyId << "\n"
                      << "tier:        " << e->tier << "\n"
                      << "fingerprint: sha256:" << e->fingerprint << "\n"
                      << "path:        " << e->path << "\n";
            return 0;
        }

        int trustVerifyCommand(int argc, const char* argv[]) {
            if (argc < 3) {
                std::cerr << "Usage: cajeta trust verify <archive>\n";
                return 2;
            }
            std::string archive = argv[2];
            auto layout = cajeta::cli::resolveTrustStoreLayout();
            auto r = cajeta::cli::verifyArchiveSignature(layout, archive);
            if (!r) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << r.takeError();
                std::cerr << msg << "\n";
                return 1;
            }
            std::cout << "ok: " << archive
                      << "  key-id=" << r->keyId
                      << "  fingerprint=sha256:" << r->fingerprint
                      << "  archive-sha256=sha256:" << r->archiveSha256
                      << "\n";
            return 0;
        }

        int trustCommand(int argc, const char* argv[]) {
            if (argc < 3 || std::string_view(argv[2]) == "--help" ||
                std::string_view(argv[2]) == "-h") {
                std::cout
                    << "Usage: cajeta trust <subcommand> [args...]\n"
                    << "\n"
                    << "  list\n"
                    << "    Print every key-id visible through the\n"
                    << "    env → user → system precedence chain.\n"
                    << "  add <key-id> <pem-path>\n"
                    << "    Copy a PEM ed25519 public key into the\n"
                    << "    user trust store. System tier untouched.\n"
                    << "  remove <key-id>\n"
                    << "    Delete a key from the user tier.\n"
                    << "  show <key-id>\n"
                    << "    Print tier + fingerprint for one key.\n"
                    << "  verify <archive>\n"
                    << "    One-shot: verify archive against the\n"
                    << "    matching trusted key (using the\n"
                    << "    <archive>.sig + <archive>.sig.keyid\n"
                    << "    sidecar).\n";
                return argc < 3 ? 1 : 0;
            }
            std::string_view sub = argv[2];
            // The handlers read their first argument at argv[2] — the
            // convention where the subcommand word has been shifted out.
            // Passing the unshifted vector made every arg-taking trust
            // subcommand parse its own name as the argument ("trust add
            // relkey k.pem" read key-id "add", pem "relkey"), so add/
            // remove/show/verify could never have worked.
            if (sub == "list")   return trustListCommand(argc - 1, argv + 1);
            if (sub == "add")    return trustAddCommand(argc - 1, argv + 1);
            if (sub == "remove") return trustRemoveCommand(argc - 1, argv + 1);
            if (sub == "show")   return trustShowCommand(argc - 1, argv + 1);
            if (sub == "verify") return trustVerifyCommand(argc - 1, argv + 1);
            std::cerr << "cajeta trust: unknown subcommand '"
                      << sub << "'\n";
            return 2;
        }

        // Resolve the effective signature-verification mode.
        //
        // Precedence (highest first):
        //   1. CAJETA_REQUIRE_SIGNATURE env (always wins; safety
        //      net — operators can pin strict regardless of CLI).
        //   2. --verify-signature[=mode] CLI flag (parsed by the
        //      dispatcher; "" means absent).
        //   3. Default: "off" for local builds.
        //
        // Returns "off" / "warn" / "strict".
        std::string resolveVerifyMode(const std::string& cliMode) {
            const char* env = std::getenv("CAJETA_REQUIRE_SIGNATURE");
            if (env && *env) {
                std::string s(env);
                if (s == "strict" || s == "warn" || s == "off") {
                    return s;
                }
            }
            if (cliMode == "strict" || cliMode == "warn" ||
                cliMode == "off") {
                return cliMode;
            }
            return "off";
        }

        // Phase 12: `cajeta workspace …` subcommand surface.
        //
        // The user can be standing anywhere inside a workspace; we
        // walk up to find the workspace root, load the workspace,
        // and iterate (or single-pick via `-p`) the members.
        //
        // Per the spec a member-task shadows the workspace-task of
        // the same name: when a member's manifest declares its own
        // task with the requested name, that wins; otherwise we
        // dispatch to the workspace-root's task definition with the
        // member's manifest as the context.
        int workspaceRunForMember(
            const Workspace& ws,
            const WorkspaceMember& m,
            const std::string& taskName) {
            // Member-defined task wins.
            auto memberTasks = parseTasks(m.manifest);
            if (!memberTasks) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << memberTasks.takeError();
                std::cerr << "cajeta workspace: member '"
                          << memberShortName(m) << "': " << msg << "\n";
                return 1;
            }
            std::string manifestForTask = m.manifestPath;
            bool memberWins = (memberTasks->count(taskName) > 0);
            if (!memberWins) {
                // Fall back to the workspace-root's task definition
                // by invoking against the workspace-root manifest
                // — the member contributes its source tree via
                // ${workspace.root}-relative paths in the action.
                manifestForTask = ws.manifestPath;
            }
            PropertyOverrides overrides;
            loadEnvOverrides(overrides);
            auto project = loadProject(manifestForTask, overrides);
            if (!project) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << project.takeError();
                std::cerr << "cajeta workspace: " << msg << "\n";
                return 1;
            }
            if (!project->tasks.count(taskName)) {
                std::cerr << "cajeta workspace: task '" << taskName
                          << "' not defined in "
                          << (memberWins ? "member '" + memberShortName(m) + "'"
                                         : "workspace root")
                          << "\n";
                return 1;
            }
            ActionRegistry registry;
            if (auto e = wireManifestPlugins(project->manifest, manifestForTask,
                                             registry)) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << std::move(e);
                std::cerr << "cajeta workspace: " << msg << "\n";
                return 1;
            }
            TaskInvocationParams cliParams;
            auto outputs = runTask(
                project->tasks, taskName, cliParams,
                project->props, registry, &project->manifest);
            if (!outputs) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << outputs.takeError();
                std::cerr << "cajeta workspace: " << memberShortName(m)
                          << ":" << taskName << ": " << msg << "\n";
                return 1;
            }
            std::cout << "[workspace] " << memberShortName(m)
                      << ":" << taskName << " — "
                      << (memberWins ? "member-defined" : "shadow of workspace task")
                      << " — OK\n";
            return 0;
        }

        int workspaceTaskCommand(
            int argc, const char* argv[],
            const std::string& taskName) {
            std::string memberFilter;
            std::string explicitRoot;
            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                std::string value;
                if (arg == "-p" && i + 1 < argc) {
                    memberFilter = argv[++i];
                } else if (match(arg, "package", value)) {
                    memberFilter = std::move(value);
                } else if (match(arg, "member", value)) {
                    memberFilter = std::move(value);
                } else if (match(arg, "manifest", value)) {
                    explicitRoot = std::move(value);
                } else if (match(arg, "diag-format", value)) {
                    if (!applyDiagFormatArg(value)) {
                        std::cerr << "cajeta workspace " << taskName
                                  << ": --diag-format must be text|json\n";
                        return 1;
                    }
                } else {
                    std::cerr << "cajeta workspace " << taskName
                              << ": unknown argument '" << arg << "'\n";
                    return 1;
                }
            }
            std::string rootPath = explicitRoot;
            if (rootPath.empty()) {
                auto found = discoverWorkspaceRoot(".");
                if (!found) {
                    std::cerr << "cajeta workspace " << taskName
                              << ": no workspace root found from cwd\n";
                    return 1;
                }
                rootPath = *found;
            }
            auto ws = loadWorkspace(rootPath);
            if (!ws) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << ws.takeError();
                std::cerr << "cajeta workspace " << taskName
                          << ": " << msg << "\n";
                return 1;
            }
            auto order = topologicallySortMembers(*ws);
            if (!order) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << order.takeError();
                std::cerr << "cajeta workspace " << taskName
                          << ": " << msg << "\n";
                return 1;
            }
            int failed = 0;
            for (const auto* m : *order) {
                if (!memberFilter.empty() &&
                    memberShortName(*m) != memberFilter) {
                    continue;
                }
                int rc = workspaceRunForMember(*ws, *m, taskName);
                if (rc != 0) ++failed;
            }
            return failed == 0 ? 0 : 1;
        }

        // Phase 14: `cajeta toolchain …` subcommands.
        //
        // The dispatch flow surfaces here too: every top-of-main
        // invocation runs `applyDispatchPolicy` before falling
        // through to the regular subcommand routing. When the
        // policy says ReExec, we execve the resolved binary; when
        // it says NeedsInstall we surface the install hint; when
        // it says Continue we just continue.
        bool parseDistVersion(const std::string& s,
                              std::string& dist,
                              std::string& version) {
            auto colon = s.find(':');
            if (colon == std::string::npos) return false;
            dist = s.substr(0, colon);
            version = s.substr(colon + 1);
            return !dist.empty() && !version.empty();
        }

        int toolchainListCommand(int /*argc*/, const char* /*argv*/[]) {
            auto layout = resolveToolchainStoreLayout();
            auto inst = listInstalledToolchains(layout);
            std::cout << "Toolchain store: " << layout.root << "\n";
            if (inst.empty()) {
                std::cout << "  (no toolchains installed)\n";
                return 0;
            }
            for (const auto& t : inst) {
                std::cout << "  " << t.distribution << ":"
                          << t.version
                          << (t.isDefault ? "  (default)" : "")
                          << "\n";
            }
            return 0;
        }

        int toolchainInstallCommand(int argc, const char* argv[]) {
            if (argc < 4) {
                std::cerr << "Usage: cajeta toolchain install "
                             "<distribution>:<version>\n";
                return 1;
            }
            std::string dist, ver;
            if (!parseDistVersion(argv[3], dist, ver)) {
                std::cerr << "cajeta toolchain install: argument must "
                             "be '<distribution>:<version>', got '"
                          << argv[3] << "'\n";
                return 1;
            }
            auto layout = resolveToolchainStoreLayout();
            auto installRoot = layout.installRoot(dist, ver);
            namespace fs = std::filesystem;
            // v1 install is "ensure the install directory exists +
            // claim it"; the actual archive fetch + verify + extract
            // step lives in the registry client (deferred slice).
            // We do create the bin/lib/share skeleton so listing +
            // dispatch wiring can be exercised end-to-end today.
            std::error_code ec;
            fs::create_directories(fs::path(installRoot) / "bin", ec);
            fs::create_directories(fs::path(installRoot) / "lib", ec);
            fs::create_directories(fs::path(installRoot) / "share", ec);
            if (ec) {
                std::cerr << "cajeta toolchain install: cannot create "
                          << installRoot << ": " << ec.message() << "\n";
                return 1;
            }
            std::cout << "Installed (skeleton) "
                      << dist << ":" << ver << " at " << installRoot
                      << "\n";
            std::cout << "  (v1: archive fetch + verify + extract is "
                         "a deferred slice; this command lays the "
                         "directory layout the dispatcher consults)\n";
            return 0;
        }

        int toolchainRemoveCommand(int argc, const char* argv[]) {
            if (argc < 4) {
                std::cerr << "Usage: cajeta toolchain remove "
                             "<distribution>:<version>\n";
                return 1;
            }
            std::string dist, ver;
            if (!parseDistVersion(argv[3], dist, ver)) {
                std::cerr << "cajeta toolchain remove: argument must "
                             "be '<distribution>:<version>'\n";
                return 1;
            }
            auto layout = resolveToolchainStoreLayout();
            auto installRoot = layout.installRoot(dist, ver);
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(installRoot, ec)) {
                std::cerr << "cajeta toolchain remove: "
                          << dist << ":" << ver
                          << " is not installed\n";
                return 1;
            }
            auto n = fs::remove_all(installRoot, ec);
            if (ec) {
                std::cerr << "cajeta toolchain remove: " << ec.message()
                          << "\n";
                return 1;
            }
            std::cout << "Removed " << dist << ":" << ver
                      << " (" << n << " entries)\n";
            return 0;
        }

        int toolchainDefaultCommand(int argc, const char* argv[]) {
            if (argc < 4) {
                std::cerr << "Usage: cajeta toolchain default "
                             "<distribution>:<version>\n";
                return 1;
            }
            std::string dist, ver;
            if (!parseDistVersion(argv[3], dist, ver)) {
                std::cerr << "cajeta toolchain default: argument must "
                             "be '<distribution>:<version>'\n";
                return 1;
            }
            auto layout = resolveToolchainStoreLayout();
            auto installRoot = layout.installRoot(dist, ver);
            namespace fs = std::filesystem;
            std::error_code ec;
            if (!fs::exists(installRoot, ec)) {
                std::cerr << "cajeta toolchain default: "
                          << dist << ":" << ver
                          << " is not installed — run 'cajeta "
                             "toolchain install " << dist << ":" << ver
                          << "' first\n";
                return 1;
            }
            auto symlink = layout.defaultSymlinkPath();
            // Refresh the symlink atomically: remove + create.
            if (fs::is_symlink(symlink, ec) || fs::exists(symlink, ec)) {
                fs::remove(symlink, ec);
            }
            fs::create_symlink(installRoot, symlink, ec);
            if (ec) {
                std::cerr << "cajeta toolchain default: cannot create "
                             "symlink " << symlink << ": "
                          << ec.message() << "\n";
                return 1;
            }
            std::cout << "Default is now " << dist << ":" << ver
                      << " (-> " << installRoot << ")\n";
            return 0;
        }

        int toolchainPinCommand(int argc, const char* argv[]) {
            if (argc < 4) {
                std::cerr << "Usage: cajeta toolchain pin "
                             "<version> [--manifest=<path>]\n";
                return 1;
            }
            std::string version = argv[3];
            std::string manifestPath = "./cajeta.json";
            for (int i = 4; i < argc; ++i) {
                std::string v;
                if (match(argv[i], "manifest", v)) {
                    manifestPath = std::move(v);
                }
            }
            // Read the manifest as raw bytes + inject/overwrite the
            // settings.toolchain.version field. We keep the rest of
            // the manifest's JSONC verbatim — same approach as the
            // existing `cajeta add` rewrite path uses for
            // settings.dependencies.
            std::ifstream in(manifestPath);
            if (!in) {
                std::cerr << "cajeta toolchain pin: cannot read '"
                          << manifestPath << "'\n";
                return 1;
            }
            std::ostringstream ss; ss << in.rdbuf();
            std::string body = ss.str();
            in.close();
            // Surgical: write a minimal settings.toolchain stub
            // adjacent to the existing settings block when present,
            // or insert one alongside details. v1 uses a simple
            // append-or-replace strategy that keeps formatting
            // local to the toolchain block.
            std::string pinJson =
                "    \"toolchain\": {\n"
                "        \"version\": \"" + version + "\"\n"
                "    }";
            // If settings.toolchain already exists, this is a
            // re-pin: replace via a substring rewrite.
            auto tcPos = body.find("\"toolchain\"");
            if (tcPos != std::string::npos) {
                // Naive: walk to the closing brace of the toolchain
                // object and replace from "toolchain" through that
                // closing brace.
                auto open = body.find('{', tcPos);
                if (open == std::string::npos) {
                    std::cerr << "cajeta toolchain pin: existing "
                                 "settings.toolchain block is malformed\n";
                    return 1;
                }
                int depth = 1;
                size_t close = open + 1;
                for (; close < body.size() && depth > 0; ++close) {
                    if (body[close] == '{') ++depth;
                    else if (body[close] == '}') --depth;
                }
                if (depth != 0) {
                    std::cerr << "cajeta toolchain pin: unbalanced "
                                 "braces in existing toolchain block\n";
                    return 1;
                }
                std::string repl =
                    "\"toolchain\": {\n"
                    "        \"version\": \"" + version + "\"\n"
                    "    }";
                body.replace(tcPos, close - tcPos, repl);
            } else {
                // No existing toolchain block. Find the closing brace
                // of settings (or insert a settings block) — v1 keeps
                // this minimal: insert a new `settings.toolchain`
                // before the last top-level '}'.
                auto lastClose = body.find_last_of('}');
                if (lastClose == std::string::npos) {
                    std::cerr << "cajeta toolchain pin: manifest has "
                                 "no top-level closing brace\n";
                    return 1;
                }
                std::string insert =
                    ",\n    \"settings\": {\n" + pinJson + "\n    }\n";
                body.insert(lastClose, insert);
            }
            std::ofstream out(manifestPath, std::ios::trunc);
            if (!out) {
                std::cerr << "cajeta toolchain pin: cannot write '"
                          << manifestPath << "'\n";
                return 1;
            }
            out << body;
            std::cout << "Pinned toolchain version=" << version
                      << " in " << manifestPath << "\n";
            return 0;
        }

        int toolchainWhichCommand(int /*argc*/, const char* /*argv*/[]) {
            std::string projectRoot =
                std::filesystem::current_path().string();
            std::unique_ptr<Manifest> mptr;
            auto manifest = loadManifestFile(projectRoot + "/cajeta.json");
            if (manifest) {
                mptr = std::make_unique<Manifest>(std::move(*manifest));
            } else {
                llvm::consumeError(manifest.takeError());
            }
            auto tc = resolveEffectiveToolchain(mptr.get(), projectRoot);
            if (!tc) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << tc.takeError();
                std::cerr << "cajeta toolchain which: " << msg << "\n";
                return 1;
            }
            auto layout = resolveToolchainStoreLayout();
            if (!tc->hasPin) {
                std::cout << "No pin in scope — running binary will "
                             "be used. Toolchain identity: "
                          << toolchainIdentity(*tc) << "\n";
                return 0;
            }
            std::cout << tc->pin.distribution << ":" << tc->pin.version
                      << " (source: " << tc->sourcePath << ")\n";
            std::cout << "  binary: "
                      << layout.binaryPath(tc->pin.distribution,
                                           tc->pin.version)
                      << "\n";
            return 0;
        }

        int toolchainShowCommand(int /*argc*/, const char* /*argv*/[]) {
            std::string projectRoot =
                std::filesystem::current_path().string();
            std::unique_ptr<Manifest> mptr;
            auto manifest = loadManifestFile(projectRoot + "/cajeta.json");
            if (manifest) {
                mptr = std::make_unique<Manifest>(std::move(*manifest));
            } else {
                llvm::consumeError(manifest.takeError());
            }
            auto tc = resolveEffectiveToolchain(mptr.get(), projectRoot);
            if (!tc) {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << tc.takeError();
                std::cerr << "cajeta toolchain show: " << msg << "\n";
                return 1;
            }
            auto layout = resolveToolchainStoreLayout();
            std::cout << "Manifest pin: ";
            if (tc->hasPin) {
                std::cout << tc->pin.distribution << ":"
                          << tc->pin.version;
                if (tc->pin.channel)
                    std::cout << " (channel=" << *tc->pin.channel << ")";
                std::cout << " — fetch=" << fetchPolicyToString(tc->pin.fetch);
                std::cout << "\n  source: " << tc->sourcePath << "\n";
            } else {
                std::cout << "(none)\n";
            }
            std::cout << "Resolved binary: "
                      << (tc->hasPin
                           ? layout.binaryPath(tc->pin.distribution,
                                               tc->pin.version)
                           : "(running PATH binary)") << "\n";
            std::cout << "Toolchain identity (IR cache discriminator): "
                      << toolchainIdentity(*tc) << "\n";
            auto decision = computeDispatchDecision(*tc, layout);
            if (decision) {
                std::cout << "Dispatch action: ";
                switch (decision->action) {
                    case DispatchAction::Continue:
                        std::cout << "continue (running binary)"; break;
                    case DispatchAction::ReExec:
                        std::cout << "re-exec into " << decision->resolvedBinaryPath; break;
                    case DispatchAction::NeedsInstall:
                        std::cout << "needs install — " << decision->installHint; break;
                }
                std::cout << "\n";
                for (const auto& note : decision->notes) {
                    std::cout << "  note: " << note << "\n";
                }
            } else {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                os << decision.takeError();
                std::cout << "Dispatch action: ERROR — " << msg << "\n";
            }
            return 0;
        }

        int toolchainCommand(int argc, const char* argv[]) {
            if (argc < 3 ||
                std::string_view(argv[2]) == "--help" ||
                std::string_view(argv[2]) == "-h") {
                std::cout
                    << "Usage: cajeta toolchain <subcommand> [options]\n"
                    << "\n"
                    << "Subcommands:\n"
                    << "  list                            list installed toolchains\n"
                    << "  install <dist>:<ver>            install a toolchain (v1: layout-only)\n"
                    << "  remove  <dist>:<ver>            remove an installed toolchain\n"
                    << "  default <dist>:<ver>            set workstation-wide default\n"
                    << "  pin     <version>               write settings.toolchain into cajeta.json\n"
                    << "  which                           print resolved binary path\n"
                    << "  show                            full dispatch + identity report\n";
                return argc < 3 ? 1 : 0;
            }
            std::string_view sub = argv[2];
            if (sub == "list")    return toolchainListCommand(argc, argv);
            if (sub == "install") return toolchainInstallCommand(argc, argv);
            if (sub == "remove")  return toolchainRemoveCommand(argc, argv);
            if (sub == "default") return toolchainDefaultCommand(argc, argv);
            if (sub == "pin")     return toolchainPinCommand(argc, argv);
            if (sub == "which")   return toolchainWhichCommand(argc, argv);
            if (sub == "show")    return toolchainShowCommand(argc, argv);
            std::cerr << "cajeta toolchain: unknown subcommand '"
                      << sub << "'\n";
            return 1;
        }

        // Phase 13: `cajeta install <archive>` — consumer-side
        // verification before extracting / installing a built
        // archive. The flow:
        //
        //   1. Compute archive sha256.
        //   2. If `<archive>.sig` exists (or `--require-signature`),
        //      verify against the trust store (Phase 10).
        //   3. If `<archive>.attestation` exists (or
        //      `--require-attestation`), verify the provenance's
        //      digest claim matches the computed sha256 + the
        //      Statement / predicate type strings are spec-shaped.
        //   4. Refuse to install when any verification step fails.
        //
        // v1 is verification-only — the install path that unpacks
        // the archive into the user's local cache lives alongside
        // the existing ArtifactCache and lands when first-party
        // package consumption flows do.
        // `cajeta install` (no archive arg): build the current project's
        // library .cja and install it into the local ~/.olla repository
        // (the Maven `mvn install` analog). Errors if the project builds an
        // executable (declares an entry-method) rather than a library.
        int installProjectMode(const std::string& ollaRoot) {
            namespace fs = std::filesystem;
            const std::string manifestPath = "./cajeta.json";
            auto m = loadManifestFile(manifestPath);
            if (!m) {
                std::string msg; llvm::raw_string_ostream os(msg);
                os << m.takeError();
                std::cerr << "cajeta install: " << msg << "\n";
                return 1;
            }
            auto sb = parseSettingsBuild(*m);
            if (!sb) {
                std::string msg; llvm::raw_string_ostream os(msg);
                os << sb.takeError();
                std::cerr << "cajeta install: " << msg << "\n";
                return 1;
            }
            if (sb->entryMethod.has_value()) {
                std::cerr << "cajeta install: only libraries can be installed "
                             "into ~/.olla; this project declares an entry-method "
                             "('" << *sb->entryMethod << "') and builds an "
                             "executable, not a .cja library\n";
                return 1;
            }
            const std::string name = m->details.name;
            const std::string version = m->details.version;

            // Build the project (its `build` task), capturing the artifact path.
            PropertyOverrides overrides;
            loadEnvOverrides(overrides);
            auto project = loadProject(manifestPath, overrides);
            if (!project) {
                std::string msg; llvm::raw_string_ostream os(msg);
                os << project.takeError();
                std::cerr << "cajeta install: " << msg << "\n";
                return 1;
            }
            ActionRegistry registry;
            TaskInvocationParams cliParams;
            auto outputs = runTask(project->tasks, "build", cliParams,
                                   project->props, registry, &project->manifest);
            if (!outputs) {
                std::string msg; llvm::raw_string_ostream os(msg);
                os << outputs.takeError();
                std::cerr << "cajeta install: build failed — " << msg << "\n";
                return 1;
            }
            std::string archivePath;
            auto it = outputs->find("path");
            if (it != outputs->end()) archivePath = it->second;
            if (archivePath.empty()) {
                std::string outDir = sb->outputDir.value_or("build");
                archivePath = outDir + "/archive/" + name + "-" + version + ".cja";
            }
            if (!fs::exists(archivePath)) {
                std::cerr << "cajeta install: built archive not found at '"
                          << archivePath << "'\n";
                return 1;
            }
            OllaStore store(ollaRoot);
            auto installed = store.write(
                name, version, archivePath,
                std::optional<std::string>(manifestPath));
            if (!installed) {
                std::string msg; llvm::raw_string_ostream os(msg);
                os << installed.takeError();
                std::cerr << "cajeta install: " << msg << "\n";
                return 1;
            }
            std::cout << "Installed " << name << "@" << version << " -> "
                      << *installed << "\n";
            return 0;
        }

        // Copy an already-verified archive into ~/.olla, deriving
        // name/version from its `<name>-<version>.cja` filename (plus an
        // optional sibling cajeta.json sidecar).
        int installArchiveIntoOlla(const std::string& ollaRoot,
                                   const std::string& archive) {
            namespace fs = std::filesystem;
            std::string stem = fs::path(archive).filename().string();
            if (stem.size() > 4 &&
                stem.compare(stem.size() - 4, 4, ".cja") == 0) {
                stem = stem.substr(0, stem.size() - 4);
            }
            auto dash = stem.rfind('-');
            if (dash == std::string::npos || dash == 0 ||
                dash + 1 >= stem.size()) {
                std::cerr << "cajeta install: cannot derive name/version from '"
                          << archive << "' (expected <name>-<version>.cja)\n";
                return 1;
            }
            std::string name = stem.substr(0, dash);
            std::string version = stem.substr(dash + 1);
            std::optional<std::string> sidecar;
            auto sc = fs::path(archive).parent_path() / "cajeta.json";
            if (fs::exists(sc)) sidecar = sc.string();
            OllaStore store(ollaRoot);
            auto installed = store.write(name, version, archive, sidecar);
            if (!installed) {
                std::string msg; llvm::raw_string_ostream os(msg);
                os << installed.takeError();
                std::cerr << "cajeta install: " << msg << "\n";
                return 1;
            }
            std::cout << "Installed " << name << "@" << version << " -> "
                      << *installed << "\n";
            return 0;
        }

        int installCommand(int argc, const char* argv[]) {
            namespace fs = std::filesystem;
            // Modes: `cajeta install` (build cwd library → ~/.olla) and
            // `cajeta install <archive.cja>` (verify the file, then → ~/.olla).
            std::string archive;
            bool requireSig = false;
            bool requireAtt = false;
            for (int i = 2; i < argc; ++i) {
                std::string_view a = argv[i];
                if (a == "--require-signature") requireSig = true;
                else if (a == "--require-attestation") requireAtt = true;
                else if (!a.empty() && a[0] == '-') {
                    std::cerr << "cajeta install: unknown argument '"
                              << a << "'\n";
                    return 1;
                } else if (archive.empty()) {
                    archive = std::string(a);
                } else {
                    std::cerr << "cajeta install: unexpected extra argument '"
                              << a << "'\n";
                    return 1;
                }
            }
            const std::string ollaRoot = OllaStore::resolveRoot();
            if (archive.empty()) {
                return installProjectMode(ollaRoot);
            }
            if (!fs::exists(archive)) {
                std::cerr << "cajeta install: archive not found: '"
                          << archive << "'\n";
                return 1;
            }
            std::ifstream in(archive, std::ios::binary);
            if (!in) {
                std::cerr << "cajeta install: cannot read '"
                          << archive << "'\n";
                return 1;
            }
            std::ostringstream ss;
            ss << in.rdbuf();
            std::string bytes = ss.str();
            std::string sha = sha256Hex(bytes);

            std::cout << "Archive:     " << archive << "\n";
            std::cout << "Size:        " << bytes.size() << " bytes\n";
            std::cout << "SHA-256:     " << sha << "\n";

            std::string sigPath = archive + ".sig";
            std::string keyIdPath = sigPath + ".keyid";
            std::string attPath = archive + ".attestation";

            bool sigPresent = fs::exists(sigPath);
            bool attPresent = fs::exists(attPath);

            if (requireSig && !sigPresent) {
                std::cerr << "cajeta install: refusing to install — "
                             "--require-signature set but '" << sigPath
                          << "' not found\n";
                return 1;
            }
            if (requireAtt && !attPresent) {
                std::cerr << "cajeta install: refusing to install — "
                             "--require-attestation set but '" << attPath
                          << "' not found\n";
                return 1;
            }

            if (sigPresent) {
                cajeta::cli::VerifyOptions opts;
                if (fs::exists(keyIdPath)) {
                    opts.signaturePathOverride = sigPath;
                }
                auto layout = cajeta::cli::resolveTrustStoreLayout();
                auto v = cajeta::cli::verifyArchiveSignature(
                    layout, archive, opts);
                if (!v) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << v.takeError();
                    std::cerr << "cajeta install: signature verify "
                                 "failed — " << msg << "\n";
                    return 1;
                }
                std::cout << "Signature:   verified (key-id "
                          << v->keyId << ")\n";
            } else {
                std::cout << "Signature:   (none — not required)\n";
            }

            if (attPresent) {
                std::ifstream af(attPath, std::ios::binary);
                std::ostringstream as;
                as << af.rdbuf();
                auto pv = verifyProvenanceJson(as.str(), sha);
                if (!pv) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << pv.takeError();
                    std::cerr << "cajeta install: attestation "
                                 "verify failed — " << msg << "\n";
                    return 1;
                }
                std::cout << "Attestation: verified (build "
                          << pv->compilerVersion;
                if (!pv->flavor.empty())
                    std::cout << " / flavor=" << pv->flavor;
                if (!pv->target.empty())
                    std::cout << " / target=" << pv->target;
                std::cout << ")\n";
            } else {
                std::cout << "Attestation: (none — not required)\n";
            }

            std::cout << "OK — archive verified\n";
            return installArchiveIntoOlla(ollaRoot, archive);
        }

        // Phase 11: `cajeta verify-reproducible <archive-a>
        // <archive-b>` — byte-compare two archives produced from
        // the same source/lockfile. Exit 0 on identical, 1 on
        // diff. Used by the CI rebuild-and-compare verifier.
        int verifyReproducibleCommand(int argc, const char* argv[]) {
            if (argc < 4) {
                std::cerr << "Usage: cajeta verify-reproducible "
                             "<archive-a> <archive-b>\n";
                return 1;
            }
            std::string a = argv[2];
            std::string b = argv[3];
            auto r = verifyReproducibleArchive(a, b);
            if (r.identical) {
                std::cout << "OK — archives are byte-identical ("
                          << r.sizeA << " bytes)\n";
                return 0;
            }
            std::cerr << "FAIL — " << r.diff << "\n";
            return 1;
        }

        // Phase 11: `cajeta sandbox-info` — diagnostic dump of the
        // sandbox layer's current view of the host. Surfaces
        // bwrap-availability + which actions get which capability
        // sets so consumers can audit their build before shipping.
        int sandboxInfoCommand(int /*argc*/, const char* /*argv*/[]) {
            std::cout << "Sandbox primitive: "
                      << (hostSandboxAvailable() ? "available" : "missing")
                      << "\n";
            const char* dbg = std::getenv("CAJETA_NO_SANDBOX");
            std::cout << "CAJETA_NO_SANDBOX:  "
                      << (dbg && *dbg ? "set (sandbox bypassed)" : "unset")
                      << "\n";
            std::cout << "\nNative action capabilities:\n";
            // Hand-listed so the output's stable; pulled from
            // nativeActionCapabilities() above.
            for (const char* name : {"exec", "copy", "delete", "mkdir",
                                      "sign", "verify-sig", "version",
                                      "download", "build", "clean", "test",
                                      "package", "upload", "publish"}) {
                auto caps = nativeActionCapabilities(name);
                std::cout << "  " << name << ": ";
                if (!caps) {
                    std::cout << "<unknown action>\n";
                    continue;
                }
                if (caps->empty()) {
                    std::cout << "(none — pure data)\n";
                    continue;
                }
                bool first = true;
                for (auto c : *caps) {
                    if (!first) std::cout << ", ";
                    std::cout << capabilityToString(c);
                    first = false;
                }
                std::cout << "\n";
            }
            return 0;
        }

        int workspaceCommand(int argc, const char* argv[]) {
            if (argc < 3 ||
                std::string_view(argv[2]) == "--help" ||
                std::string_view(argv[2]) == "-h") {
                std::cout
                    << "Usage: cajeta workspace <subcommand> [options]\n"
                    << "\n"
                    << "Subcommands:\n"
                    << "  build   [-p <member>]       build every member "
                       "(or one) in topological order\n"
                    << "  publish [-p <member>]       publish every member "
                       "(or one)\n"
                    << "  test    [-p <member>]       run the test task on "
                       "every member (or one)\n"
                    << "  members                     list workspace members\n"
                    << "\n"
                    << "Options:\n"
                    << "  --manifest=<path>           workspace-root "
                       "manifest path (default: discover via ancestors)\n";
                return argc < 3 ? 1 : 0;
            }
            std::string_view sub = argv[2];
            int subArgc = argc - 1;
            const char** subArgv = argv + 1;
            // subArgv[0] is now the original argv[1] ("workspace")
            // — the callee uses it for diagnostics, so let it stand.
            if (sub == "build")   return workspaceTaskCommand(subArgc, subArgv, "build");
            if (sub == "publish") return workspaceTaskCommand(subArgc, subArgv, "publish");
            if (sub == "test")    return workspaceTaskCommand(subArgc, subArgv, "test");
            if (sub == "members") {
                std::string explicitRoot;
                for (int i = 3; i < argc; ++i) {
                    std::string v;
                    if (match(argv[i], "manifest", v)) explicitRoot = std::move(v);
                }
                std::string rootPath = explicitRoot;
                if (rootPath.empty()) {
                    auto found = discoverWorkspaceRoot(".");
                    if (!found) {
                        std::cerr << "cajeta workspace members: "
                                     "no workspace root found from cwd\n";
                        return 1;
                    }
                    rootPath = *found;
                }
                auto ws = loadWorkspace(rootPath);
                if (!ws) {
                    std::string msg;
                    llvm::raw_string_ostream os(msg);
                    os << ws.takeError();
                    std::cerr << "cajeta workspace members: "
                              << msg << "\n";
                    return 1;
                }
                std::cout << "Workspace root: " << ws->rootPath << "\n";
                for (const auto& m : ws->members) {
                    std::cout << "  " << memberShortName(m)
                              << "  ->  " << m.declaredPath << "\n";
                }
                return 0;
            }
            std::cerr << "cajeta workspace: unknown subcommand '"
                      << sub << "'\n";
            return 1;
        }

        // True for the global flags BuildTool.md says EVERY built-in accepts.
        // `wantsValue` reports the separated forms that swallow the next token.
        bool isGlobalFlag(std::string_view arg, bool& wantsValue) {
            wantsValue = false;
            if (arg == "-P" || arg == "--property") { wantsValue = true; return true; }
            if (arg == "-v" || arg == "--verbose" || arg == "--quiet") return true;
            if (arg.rfind("--manifest", 0) == 0) return true;
            if (arg.rfind("--profile", 0) == 0) return true;
            if (arg.rfind("--property", 0) == 0) return true;
            return false;
        }

        int coverageCommand(int argc, const char* argv[]) {
            // Locate the subcommand PAST any leading global flags.
            //
            // BuildTool.md: "Every built-in subcommand and every task accepts
            // --manifest=<path>, -v/--verbose, --quiet, --profile=<name>,
            // -P <prop>=<value>". This command took argv[2] as the subcommand
            // verbatim, so the IDE's build-tool window — which passes
            // --manifest per that contract — got
            //
            //   unknown subcommand '--manifest=/…/cajeta.json'
            //
            // naming a flag as if the user had typed it as a verb.
            std::vector<const char*> rest;   // globals + subcommand args
            std::string_view sub;
            int subIndex = -1;
            for (int i = 2; i < argc; ++i) {
                std::string_view arg = argv[i];
                bool wantsValue = false;
                if (isGlobalFlag(arg, wantsValue)) {
                    rest.push_back(argv[i]);
                    if (wantsValue && i + 1 < argc) rest.push_back(argv[++i]);
                    continue;
                }
                if (subIndex < 0 && !arg.empty() && arg.front() != '-') {
                    sub = arg;
                    subIndex = i;
                    continue;
                }
                rest.push_back(argv[i]);
            }

            const bool wantsHelp = [&] {
                for (int i = 2; i < argc; ++i) {
                    std::string_view a = argv[i];
                    if (a == "--help" || a == "-h") return true;
                }
                return false;
            }();

            // Only the NO-SUBCOMMAND case is answered here. `coverage list
            // --help` must reach coverageListCommand so it prints ITS usage —
            // intercepting every --help made all three subcommands answer with
            // the generic text, and made `coverage --help` exit 1 because the
            // return folded the two cases together.
            if (subIndex < 0) {
                std::ostream& os = wantsHelp ? std::cout : std::cerr;
                os  << "Usage: cajeta coverage <subcommand> [options]\n"
                    << "\n"
                    << "Subcommands:\n"
                    << "  ignore   Add a typed exclude entry.\n"
                    << "  list     Print the current exclude entries.\n"
                    << "  remove   Remove entries by pattern.\n"
                    << "\n"
                    << "Run `cajeta coverage <subcommand> --help` for "
                    << "subcommand-specific options.\n";
                // `coverage` manages the exclude CONFIG; it does not measure
                // anything. Say so, because "Coverage" in a menu reads like it
                // should run a coverage pass.
                os << "\n"
                   << "This subcommand edits the exclude list in "
                      "cajeta.json. To MEASURE coverage, bind the "
                      "cajeta.coverage.instrument / .report actions to a "
                      "task and run that task.\n";
                // Asking for help is a success; omitting the verb is not.
                return wantsHelp ? 0 : 1;
            }

            // Re-lay the argv so the subcommand sits at index 2, which is
            // where each handler starts scanning its own options.
            std::vector<const char*> forwarded;
            forwarded.reserve(rest.size() + 3);
            forwarded.push_back(argv[0]);
            forwarded.push_back(argv[1]);
            forwarded.push_back(argv[subIndex]);
            for (const char* a : rest) forwarded.push_back(a);
            const int fargc = static_cast<int>(forwarded.size());
            const char** fargv = forwarded.data();

            if (sub == "ignore") return coverageIgnoreCommand(fargc, fargv);
            if (sub == "list")   return coverageListCommand(fargc, fargv);
            if (sub == "remove") return coverageRemoveCommand(fargc, fargv);
            std::cerr << "cajeta coverage: unknown subcommand '"
                      << sub << "' (expected: ignore, list, remove)\n";
            return 1;
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
                cmd == "upgrade" || cmd == "coverage" ||
                cmd == "publish" || cmd == "trust" ||
                cmd == "workspace" ||
                cmd == "verify-reproducible" ||
                cmd == "sandbox-info" ||
                cmd == "install" ||
                cmd == "toolchain" ||
                cmd == "kernel" ||  // jupyter-kernel §3 — first-class verb
                cmd == "run") {   // script-units §7 — first-class verb
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

        // ─── skill discovery (skill-discovery spec §1.5.1) ───────────────
        // Thin adapters over the transport-agnostic skill core
        // (cajeta::buildtool::skill): parse args, load the lockfile + local
        // artifact cache into a search context, call the core, print. No
        // business logic here, so an MCP adapter (spec §6) reuses the same core.

        std::vector<std::string> skillArgvTail(int argc, const char* argv[]) {
            std::vector<std::string> out;
            for (int i = 1; i < argc; ++i) out.push_back(argv[i]);
            return out; // out[0] is the subcommand name
        }

        // --json structured output for the skill subcommands (consumed by the
        // standalone tools/mcp wrapper). Removes "--json" from `tail`, returns
        // whether it was present.
        bool takeJsonFlag(std::vector<std::string>& tail) {
            for (auto it = tail.begin(); it != tail.end(); ++it) {
                if (*it == "--json") { tail.erase(it); return true; }
            }
            return false;
        }

        // JSON shapes live in SkillCli (shared with compiler-mcp for parity).
        std::string searchResultsJson(
                llvm::ArrayRef<skill::SkillSearchResult> rs) {
            return skill::searchResultsJsonValue(rs).dump();
        }

        std::string listEntriesJson(llvm::ArrayRef<skill::SkillListEntry> es) {
            return skill::listEntriesJsonValue(es).dump();
        }

        std::string getResultsJson(llvm::ArrayRef<skill::SkillGetResult> rs) {
            return skill::getResultsJsonValue(rs).dump();
        }

        int searchSkillCommand(int argc, const char* argv[]) {
            auto tail = skillArgvTail(argc, argv);
            bool json = takeJsonFlag(tail);
            auto a = skill::parseSearchSkillArgs(tail);
            if (!a.valid) { std::cerr << skill::searchSkillUsage(); return 2; }
            // Lockfile is optional: with none, discovery still returns the
            // always-available embedded stdlib skills (spec §2.5).
            std::vector<ResolvedPackageEntry> packages;
            if (std::filesystem::exists("./cajeta.lock")) {
                auto lf = readLockfile("./cajeta.lock");
                if (!lf) {
                    std::cerr << "cajeta search-skill: " << llvm::toString(lf.takeError())
                              << "\n";
                    return 1;
                }
                packages = std::move(lf->packagesTyped);
            }
            ArtifactCache cache(".");
            auto ctx = skill::loadSkillSearchContext(
                packages,
                [&](llvm::StringRef s) { return cache.lookup(s.str()); });
            if (!ctx) {
                std::cerr << "cajeta search-skill: " << llvm::toString(ctx.takeError())
                          << "\n";
                return 1;
            }
            skill::MatchOptions opts;
            opts.exact = a.exact;
            auto results = skill::searchSkills(a.name, a.version, a.from, *ctx, opts);
            if (json) std::cout << searchResultsJson(results) << "\n";
            else std::cout << skill::formatSearchResults(results);
            return results.empty() ? 1 : 0;
        }

        int listSkillsCommand(int argc, const char* argv[]) {
            auto tail = skillArgvTail(argc, argv);
            bool json = takeJsonFlag(tail);
            auto a = skill::parseListSkillsArgs(tail);
            if (!a.valid) { std::cerr << skill::listSkillsUsage(); return 2; }
            // Lockfile optional (spec §2.5): embedded stdlib skills always listed.
            std::vector<ResolvedPackageEntry> packages;
            if (std::filesystem::exists("./cajeta.lock")) {
                auto lf = readLockfile("./cajeta.lock");
                if (!lf) {
                    std::cerr << "cajeta list-skills: " << llvm::toString(lf.takeError())
                              << "\n";
                    return 1;
                }
                packages = std::move(lf->packagesTyped);
            }
            ArtifactCache cache(".");
            auto ctx = skill::loadSkillSearchContext(
                packages,
                [&](llvm::StringRef s) { return cache.lookup(s.str()); });
            if (!ctx) {
                std::cerr << "cajeta list-skills: " << llvm::toString(ctx.takeError())
                          << "\n";
                return 1;
            }
            auto entries = skill::listSkills(a.scope, a.version, a.from, *ctx);
            if (json) std::cout << listEntriesJson(entries) << "\n";
            else std::cout << skill::formatListEntries(entries);
            return 0;
        }

        int getSkillsCommand(int argc, const char* argv[]) {
            auto tail = skillArgvTail(argc, argv);   // tail[0] == "get-skills"
            bool json = takeJsonFlag(tail);
            if (tail.size() < 2) { std::cerr << skill::getSkillsUsage(); return 2; }
            auto uris = skill::splitCommaUris(tail[1]);
            if (uris.empty()) { std::cerr << skill::getSkillsUsage(); return 2; }
            // Lockfile optional (spec §2.5): embedded stdlib URIs resolve with none.
            std::vector<ResolvedPackageEntry> packages;
            if (std::filesystem::exists("./cajeta.lock")) {
                auto lf = readLockfile("./cajeta.lock");
                if (!lf) {
                    std::cerr << "cajeta get-skills: " << llvm::toString(lf.takeError())
                              << "\n";
                    return 1;
                }
                packages = std::move(lf->packagesTyped);
            }
            ArtifactCache cache(".");
            auto results = skill::getSkills(
                uris, packages,
                [&](llvm::StringRef s) { return cache.lookup(s.str()); });
            bool anyErr = false;
            for (const auto& r : results) if (!r.ok()) anyErr = true;
            if (json) {
                std::cout << getResultsJson(results) << "\n";
                return anyErr ? 1 : 0;
            }
            for (const auto& r : results) {
                std::cout << "# " << r.uri << "\n";
                if (r.ok()) {
                    std::cout << r.payload << "\n";
                } else {
                    std::cerr << "cajeta get-skills: " << r.uri << ": " << r.error
                              << "\n";
                }
            }
            return anyErr ? 1 : 0;
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
        if (cmd == "coverage") {
            *exitCodeOut = coverageCommand(argc, argv);
            return true;
        }
        if (cmd == "publish") {
            *exitCodeOut = publishCommand(argc, argv);
            return true;
        }
        if (cmd == "trust") {
            *exitCodeOut = trustCommand(argc, argv);
            return true;
        }
        if (cmd == "workspace") {
            *exitCodeOut = workspaceCommand(argc, argv);
            return true;
        }
        if (cmd == "verify-reproducible") {
            *exitCodeOut = verifyReproducibleCommand(argc, argv);
            return true;
        }
        if (cmd == "sandbox-info") {
            *exitCodeOut = sandboxInfoCommand(argc, argv);
            return true;
        }
        if (cmd == "install") {
            *exitCodeOut = installCommand(argc, argv);
            return true;
        }
        if (cmd == "toolchain") {
            *exitCodeOut = toolchainCommand(argc, argv);
            return true;
        }
        if (cmd == "search-skill") {
            *exitCodeOut = searchSkillCommand(argc, argv);
            return true;
        }
        if (cmd == "list-skills") {
            *exitCodeOut = listSkillsCommand(argc, argv);
            return true;
        }
        if (cmd == "get-skills") {
            *exitCodeOut = getSkillsCommand(argc, argv);
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
