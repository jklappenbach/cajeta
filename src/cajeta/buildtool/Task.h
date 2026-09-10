// Cajeta build-tool task model: the manifest `tasks` block and its JSON document.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // What the task accepts from the CLI.
    struct TaskParamSpec {
        std::string name;
        std::string type = "string";       // "string" | "bool"
        std::optional<std::string> defaultValue;
        bool required = false;
        std::optional<std::string> doc;
    };

    // Plain action invocation: { "action": "<name>", ... }.
    struct ActionInvocation {
        std::string action;              // action name (e.g. "exec", "build")
        std::string id;                  // optional; id under which outputs are exposed
        llvm::json::Object params;

        std::optional<std::string> whenExpr;
        std::optional<std::string> skipWhenExpr;
    };

    // run-task entry; the called task's outputs become this entry's outputs.
    struct RunTaskCall {
        std::string taskName;
        std::string id;
        std::map<std::string, std::string> params;

        std::optional<std::string> whenExpr;
        std::optional<std::string> skipWhenExpr;
    };

    // Children run concurrently; their outputs merge back after all complete.
    struct ParallelGroup;
    using ParallelGroupPtr = std::shared_ptr<ParallelGroup>;

    // One entry in a task's `actions` array; a tagged union of the three shapes.
    struct ActionEntry {
        enum class Kind { Invocation, Parallel, RunTask };
        Kind kind = Kind::Invocation;

        ActionInvocation invocation;         // valid when kind == Invocation
        ParallelGroupPtr   parallel;          // valid when kind == Parallel
        RunTaskCall        runTask;           // valid when kind == RunTask
    };

    struct ParallelGroup {
        std::vector<ActionEntry> children;
    };

    struct Task {
        std::string name;
        std::optional<std::string> description;
        std::vector<std::string> dependsOn;
        std::vector<TaskParamSpec> params;
        std::vector<ActionEntry> actions;
        std::map<std::string, std::string> outputs;
        std::optional<std::string> workingDir;
        std::map<std::string, std::string> env;
    };

    // Parse the `tasks` block from a manifest, keyed by task name.
    // Errors on malformed task entries.
    llvm::Expected<std::map<std::string, Task>> parseTasks(
        const llvm::json::Object& tasksBlock);

    llvm::Expected<std::map<std::string, Task>> parseTasks(
        const Manifest& manifest);

    // Validate the task graph for cycles in `depends-on`. Returns an Error
    // naming the cycle members in order, or success when the graph is acyclic.
    llvm::Error validateTaskGraph(const std::map<std::string, Task>& tasks);

    // A built-in subcommand the tool exposes (init, add, info, ...).
    struct BuiltinCommand {
        std::string name;
        std::string description;
    };

    // Project-level debug-launch coordinates: `cajeta dap` JIT-runs `entryMethod`
    // from `sourceRoot`, so a Debug launch is formed from these, not from a build.
    struct DebugLaunchCoords {
        std::optional<std::string> sourceRoot;
        std::optional<std::string> entryMethod;
    };

    // Render the `cajeta tasks --json` document. Pure, pretty-printed, tasks in
    // map order. The `build` object is emitted only when both debug-launch
    // coordinates are known, so the IDE disables Debug otherwise.
    std::string renderTasksJson(const std::string& manifestPath,
                                const std::map<std::string, Task>& tasks,
                                const std::vector<BuiltinCommand>& builtins,
                                const DebugLaunchCoords& debugCoords = {});

} // namespace cajeta::buildtool
