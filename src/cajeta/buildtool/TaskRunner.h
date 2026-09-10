// Cajeta build-tool task execution: walks a task's `actions` in order, substitutes params,
// invokes each action and threads outputs through `${id.field}`, aborting on first failure.
#pragma once

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Task.h"

#include <llvm/Support/Error.h>

#include <map>
#include <ostream>
#include <string>

namespace cajeta::buildtool {

    // CLI-supplied param bindings, keyed by the name declared in the task's `params` block.
    struct TaskInvocationParams {
        std::map<std::string, std::string> values;
    };

    // Execute one task end-to-end and return its resolved `outputs` block. `depends-on`
    // prerequisites run first, at most once each, and the graph is checked for cycles before
    // any action fires. `manifest` reaches TaskContext and may be null in unit tests.
    llvm::Expected<std::map<std::string, std::string>> runTask(
        const std::map<std::string, Task>& tasks,
        const std::string& taskName,
        const TaskInvocationParams& cliParams,
        const ResolvedProperties& props,
        const ActionRegistry& registry,
        const Manifest* manifest = nullptr);

    // Print the resolved action sequence for a task without running it (`cajeta task --show`).
    // A reference that cannot be resolved prints as the literal `${name}`.
    llvm::Error showTask(
        const std::map<std::string, Task>& tasks,
        const std::string& taskName,
        const TaskInvocationParams& cliParams,
        const ResolvedProperties& props,
        std::ostream& out,
        const Manifest* manifest = nullptr);

} // namespace cajeta::buildtool
