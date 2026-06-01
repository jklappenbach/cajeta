// Cajeta build-tool action runtime.
//
// An Action is an executable verb invoked from a task. Each
// Action ships its own implementation; the registry holds the
// catalog by name (`exec`, `build`, `copy`, ...). Phase 3a
// ships the infrastructure + `exec`; the rest of the catalog
// lands in subsequent phases per plan/build-tool-plan.md.

#pragma once

#include "cajeta/buildtool/Properties.h"
#include "cajeta/buildtool/Task.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // The lookup table feeding ${...} substitution during action
    // invocation. Combines:
    //   1. resolved manifest properties (Properties.h)
    //   2. the current task's params (CLI-bound + defaults)
    //   3. outputs published by prior actions in the same task,
    //      keyed by `${id.field}`
    //
    // Cleared / rebuilt per task invocation.
    class TaskContext {
    public:
        TaskContext(const ResolvedProperties& props,
                    const Manifest* manifest = nullptr);

        // The manifest the task is running against. Null if the
        // context was constructed without one (e.g. for unit tests
        // that don't need build-action functionality).
        const Manifest* manifest() const { return manifest_; }

        // Bind a task parameter. Available as ${params.<name>}.
        void setParam(const std::string& name, const std::string& value);

        // Publish an action's outputs under its `id`. Available as
        // ${id.<field>}.
        void publishOutputs(const std::string& id,
                            const std::map<std::string, std::string>& outputs);

        // Look up a property name following the ${...} rules.
        std::optional<std::string> lookup(const std::string& name) const;

        // Substitute every ${...} reference in a string. Errors when
        // a reference doesn't resolve.
        llvm::Expected<std::string> substitute(
            const std::string& s,
            const std::string& whereContext) const;

        // Take a snapshot of the current context. Mutations on the
        // snapshot do not propagate to the original; the snapshot
        // sees a frozen view of params + already-published outputs.
        // Used to give parallel-group children isolated contexts that
        // merge back into the parent after they all complete.
        TaskContext snapshot() const;

        // Merge another context's published action outputs into this
        // one (parallel-group join). Caller is responsible for
        // ordering merges so the final state is deterministic — by
        // convention, this is "merge children in declaration order
        // after all children join."
        void mergeOutputs(const TaskContext& other);

    private:
        const ResolvedProperties& props_;
        const Manifest* manifest_;
        std::map<std::string, std::string> params_;
        // id → outputs map; outputs is field-name → value
        std::map<std::string, std::map<std::string, std::string>> actionOutputs_;
    };

    // The result of running one action invocation.
    struct ActionResult {
        // Outputs the action published. Get exposed under the
        // invocation's `id` via TaskContext::publishOutputs.
        std::map<std::string, std::string> outputs;
        // Captured stdout/stderr (for actions like exec that produce
        // them; empty otherwise). Used by --verbose flows.
        std::string stdoutLog;
        std::string stderrLog;
    };

    // Per-action contract. Each native action is a subclass.
    class Action {
    public:
        virtual ~Action() = default;

        // The action's name as it appears in `{"action": "<name>"}`.
        virtual std::string name() const = 0;

        // Execute the action. `params` is the action's invocation
        // params with `${...}` already substituted; `ctx` is the
        // task context (for logging / further substitution if the
        // action does anything fancy). Errors abort the task.
        virtual llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const = 0;
    };

    // Registry of action names → implementations. Phase 3a registers
    // built-ins on construction; later phases add more.
    class ActionRegistry {
    public:
        ActionRegistry();

        // Look up an action by name. Returns nullptr if not registered.
        const Action* get(const std::string& name) const;

        // List the registered action names (for `cajeta tasks` /
        // `cajeta task --show` output).
        std::vector<std::string> list() const;

    private:
        std::map<std::string, std::unique_ptr<Action>> actions_;
    };

} // namespace cajeta::buildtool
