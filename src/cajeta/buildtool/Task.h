// Cajeta build-tool task model.
//
// A task is a named, declarative sequence of action invocations
// the user runs via `cajeta <task>`. See BuildTool.md "Tasks"
// section for the spec, plans/buildtool/build-tool-plan.md Phase 3 for
// context. Phase 3a (this file) models linear-execution tasks;
// depends-on / parallel / run-task / when-skip-when composition
// land in Phase 3b.

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

    // Task parameter spec — what the task accepts from the CLI.
    // Phase 3a recognizes type, default, required, doc.
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
        // Raw params as the JSON object form; resolved at invocation
        // time using the TaskContext's substitution.
        llvm::json::Object params;

        std::optional<std::string> whenExpr;
        std::optional<std::string> skipWhenExpr;
    };

    // run-task entry: { "run-task": "<task>", "params": {...}, "id": "..." }.
    // Invokes another task by name; the called task's outputs become
    // this entry's outputs (so `${id.field}` reads the called task's
    // outputs).
    struct RunTaskCall {
        std::string taskName;
        std::string id;
        // Param bindings to pass to the called task. Values are raw
        // strings (with ${...} references the runner resolves at
        // invocation time against the calling task's context).
        std::map<std::string, std::string> params;

        std::optional<std::string> whenExpr;
        std::optional<std::string> skipWhenExpr;
    };

    // parallel group: { "parallel": [...] }. Children run
    // concurrently; their outputs merge back into the calling task's
    // context after every child completes.
    struct ParallelGroup;
    using ParallelGroupPtr = std::shared_ptr<ParallelGroup>;

    // One entry in a task's `actions` array. Tagged union of the
    // three shapes; the parser determines kind from the entry's
    // structure.
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

    // A named task.
    struct Task {
        std::string name;
        std::optional<std::string> description;
        std::vector<std::string> dependsOn;
        std::vector<TaskParamSpec> params;
        std::vector<ActionEntry> actions;
        // Outputs the task exposes to callers (via run-task).
        std::map<std::string, std::string> outputs;
        std::optional<std::string> workingDir;
        std::map<std::string, std::string> env;
    };

    // Parse the `tasks` block from a manifest. Returns map keyed by
    // task name. Errors on malformed task entries.
    llvm::Expected<std::map<std::string, Task>> parseTasks(
        const llvm::json::Object& tasksBlock);

    // Convenience: pull `tasks` out of a Manifest and parse.
    llvm::Expected<std::map<std::string, Task>> parseTasks(
        const Manifest& manifest);

    // Validate the task graph for cycles in `depends-on` references.
    // Returns an Error naming the cycle members in order, or success
    // when the graph is acyclic.
    llvm::Error validateTaskGraph(const std::map<std::string, Task>& tasks);

    // A built-in subcommand the tool exposes (init, add, info, …), surfaced in
    // the `cajeta tasks --json` document alongside manifest tasks (spec §3.1.2).
    struct BuiltinCommand {
        std::string name;
        std::string description;
    };

    // Project-level debug-launch coordinates, surfaced at the document root as a
    // `build` object (widget spec §5.2.2, unit 7). These come from the manifest's
    // `settings.build` and are exactly what `cajeta dap` consumes: it JIT-runs an
    // `entryMethod` from a `sourceRoot` — it does NOT load a prebuilt artifact.
    // So a runnable task's Debug launch is formed from these, not from the build
    // action's output-path. Both must be known to be debuggable.
    struct DebugLaunchCoords {
        std::optional<std::string> sourceRoot;
        std::optional<std::string> entryMethod;
    };

    // Render the `cajeta tasks --json` document (buildtool-widget spec §3):
    // { manifest, build?{sourceRoot,entryMethod}, tasks[{name,description?,
    //   dependsOn[],params[],runnable,artifact?}], builtins[] }.
    // Tasks emit in `tasks` map order (sorted by name). Pure — no I/O — so the
    // IDE-contract shape is golden-testable. Pretty-printed (2-space). The
    // `build` object is emitted only when both debug-launch coordinates are
    // known (entryMethod present); omitted otherwise so the IDE disables Debug.
    std::string renderTasksJson(const std::string& manifestPath,
                                const std::map<std::string, Task>& tasks,
                                const std::vector<BuiltinCommand>& builtins,
                                const DebugLaunchCoords& debugCoords = {});

} // namespace cajeta::buildtool
