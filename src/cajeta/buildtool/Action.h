// Cajeta build-tool action runtime: an Action is an executable verb invoked
// from a task, and the registry holds the catalog by name.

#pragma once

#include "cajeta/buildtool/Properties.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cajeta::buildtool {

    // The ${...} lookup table for one task invocation: resolved manifest
    // properties, the task's own params, and the outputs prior actions published
    // under `${id.field}`. Rebuilt per invocation.
    class TaskContext {
    public:
        TaskContext(const ResolvedProperties& props,
                    const Manifest* manifest = nullptr);

        const Manifest* manifest() const { return manifest_; }

        const ResolvedProperties& properties() const { return props_; }

        // Binds a task parameter, reachable as ${params.<name>}.
        void setParam(const std::string& name, const std::string& value);

        // CLI `-p name=value` overrides, overlaid onto EVERY action invocation's
        // params so a builtin action's params are reachable without the task
        // declaring them. This task's context only; a called task gets its own.
        void setCliParams(const std::map<std::string, std::string>& values);
        const std::map<std::string, std::string>& cliParams() const { return cliParams_; }

        // Publishes an action's outputs under its `id`, as ${id.<field>}.
        void publishOutputs(const std::string& id,
                            const std::map<std::string, std::string>& outputs);

        // Looks a name up following the ${...} resolution rules.
        std::optional<std::string> lookup(const std::string& name) const;

        // Substitutes every ${...} in `s`; an unresolved one errors, citing
        // `whereContext` as the site.
        llvm::Expected<std::string> substitute(
            const std::string& s,
            const std::string& whereContext) const;

        // A frozen copy whose mutations do not propagate back; parallel-group
        // children each get one and merge into the parent once all complete.
        TaskContext snapshot() const;

        // The parallel-group join. The caller orders merges for determinism — by
        // convention, children in declaration order once all have joined.
        void mergeOutputs(const TaskContext& other);

    private:
        const ResolvedProperties& props_;
        const Manifest* manifest_;
        std::map<std::string, std::string> params_;
        std::map<std::string, std::string> cliParams_;
        std::map<std::string, std::map<std::string, std::string>> actionOutputs_;
    };

    // One structured finding from an action: plugins stream them as
    // `{"kind": "finding", ...}` stdout records, native actions fill them in
    // directly. Severity "error" blocks the task; "warning" and "info" do not.
    struct ActionFinding {
        std::string rule;
        std::string severity;   // "error" | "warning" | "info"
        std::string file;
        int line = 0;
        int column = 0;
        std::string message;
    };

    struct ActionResult {
        // Exposed under the invocation's `id` by TaskContext::publishOutputs.
        std::map<std::string, std::string> outputs;
        std::string stdoutLog;
        std::string stderrLog;
        // The lint task aggregates these; the test task gates on the count.
        std::vector<ActionFinding> findings;
    };

    // Per-action contract. Each native action is a subclass.
    class Action {
    public:
        virtual ~Action() = default;

        // The action's name as it appears in `{"action": "<name>"}`.
        virtual std::string name() const = 0;

        // Runs the action: `params` is the invocation's params with `${...}`
        // already substituted, `ctx` the task context. An error aborts the task.
        virtual llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const = 0;
    };

    // Registry of action names to implementations; built-ins on construction.
    class ActionRegistry {
    public:
        ActionRegistry();

        // The action registered under `name`, or nullptr.
        const Action* get(const std::string& name) const;

        std::vector<std::string> list() const;

        // Registers an action by name, silently overwriting a prior registration:
        // plugin-action collisions have already surfaced in resolvePlugins.
        void registerAction(std::unique_ptr<Action> action);

    private:
        std::map<std::string, std::unique_ptr<Action>> actions_;
    };

} // namespace cajeta::buildtool
