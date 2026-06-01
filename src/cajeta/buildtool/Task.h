// Cajeta build-tool task model.
//
// A task is a named, declarative sequence of action invocations
// the user runs via `cajeta <task>`. See BuildTool.md "Tasks"
// section for the spec, plan/build-tool-plan.md Phase 3 for
// context. Phase 3a (this file) models linear-execution tasks;
// depends-on / parallel / run-task / when-skip-when composition
// land in Phase 3b.

#pragma once

#include "cajeta/buildtool/Manifest.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <map>
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

    // A single entry in the task's `actions` array. Phase 3a only
    // models the plain action-invocation form (one action name + its
    // params). Phase 3b adds parallel-group and run-task entries.
    struct ActionInvocation {
        std::string action;              // action name (e.g. "exec", "build")
        std::string id;                  // optional; id under which outputs are exposed
        // Raw params as the JSON object form; resolved at invocation
        // time using the TaskContext's substitution.
        llvm::json::Object params;

        // when / skip-when expressions (Phase 3b actually evaluates;
        // 3a parses and stores).
        std::optional<std::string> whenExpr;
        std::optional<std::string> skipWhenExpr;
    };

    // A named task.
    struct Task {
        std::string name;
        std::optional<std::string> description;
        std::vector<std::string> dependsOn;   // Phase 3b honors; 3a parses.
        std::vector<TaskParamSpec> params;
        std::vector<ActionInvocation> actions;
        // Outputs the task exposes to callers (via run-task in 3b).
        // Each value is an expression evaluated at the end of the
        // task using the TaskContext.
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

} // namespace cajeta::buildtool
