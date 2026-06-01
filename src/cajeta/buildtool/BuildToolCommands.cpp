#include "cajeta/buildtool/BuildToolCommands.h"

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/JsonC.h"
#include "cajeta/buildtool/Lockfile.h"
#include "cajeta/buildtool/Manifest.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Task.h"
#include "cajeta/buildtool/TaskRunner.h"

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
                                  project->props, std::cout)) {
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
                project->props, registry);
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
                cmd == "archive") {
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
        if (looksLikeTaskInvocation(argc, argv)) {
            *exitCodeOut = runTaskCommand(argc, argv);
            return true;
        }

        // Other build-tool subcommands land in subsequent phases. For
        // now anything not recognized falls through to the compiler.
        return false;
    }

} // namespace cajeta::buildtool
