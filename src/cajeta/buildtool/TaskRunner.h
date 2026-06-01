// Cajeta build-tool task execution.
//
// Phase 3a: linear executor. Walks a task's `actions` array in
// order, substitutes params, invokes each action, threads outputs
// through `${id.field}`. Aborts on the first action failure
// (until `--continue-on-error` lands in Phase 3b).
//
// Phase 3b extends to: depends-on DAG traversal, parallel groups,
// run-task composition, cycle detection, when/skip-when gating.

#pragma once

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Task.h"

#include <llvm/Support/Error.h>

#include <map>
#include <ostream>
#include <string>

namespace cajeta::buildtool {

    // CLI-supplied param bindings for the invoked task. Keyed by
    // param name as declared in the task's `params` block.
    struct TaskInvocationParams {
        std::map<std::string, std::string> values;
    };

    // Execute one task end-to-end.
    //
    // - `tasks` is the full task table.
    // - `taskName` selects which task to run.
    // - `cliParams` carries CLI-supplied `-p name=value` bindings.
    // - `props` is the resolved property table from
    //   `resolveProperties()`.
    // - `registry` provides the action implementations.
    //
    // Returns the task's resolved `outputs` block (with
    // substitutions applied) on success.
    //
    // Honors `depends-on` by transitively executing each prerequisite
    // before this task (each runs at most once per invocation).
    // Validates the task graph for cycles at entry — running a task
    // whose graph has a cycle errors before any action fires.
    //
    // `manifest` is passed through to TaskContext so actions like
    // `build` can read `settings.build.binaries` and other
    // manifest-level defaults. May be nullptr for unit tests that
    // don't exercise manifest-aware actions.
    llvm::Expected<std::map<std::string, std::string>> runTask(
        const std::map<std::string, Task>& tasks,
        const std::string& taskName,
        const TaskInvocationParams& cliParams,
        const ResolvedProperties& props,
        const ActionRegistry& registry,
        const Manifest* manifest = nullptr);

    // Print the resolved action sequence for a task without actually
    // running it. Used by `cajeta task <name> --show`. Substitutes
    // every ${...} reference it can given the task's params (which
    // get their declared defaults) plus the manifest properties;
    // unresolvable references print as `${name}` literal so the
    // reader can still see what the task wanted.
    llvm::Error showTask(
        const std::map<std::string, Task>& tasks,
        const std::string& taskName,
        const TaskInvocationParams& cliParams,
        const ResolvedProperties& props,
        std::ostream& out,
        const Manifest* manifest = nullptr);

} // namespace cajeta::buildtool
