// Action subclass that wraps one plugin-provided action.
//
// One PluginAction instance lives in the ActionRegistry per (plugin,
// action-name) pair. Its run() forwards to PluginRuntime::invoke.
// Keeping plugin actions behind the same Action surface as natives
// lets the TaskRunner stay polymorphic — no "is this native or
// plugin?" branch at task-execution time.

#pragma once

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Plugin.h"

#include <memory>
#include <string>

namespace cajeta::buildtool {

    class PluginAction : public Action {
    public:
        // `defaults` is the plugin's manifest `config` block — the default
        // parameter layer Plugin.h documents ("forwards this to the
        // plugin's actions as default param values"). Explicit task params
        // overlay it at run().
        PluginAction(ResolvedPlugin plugin, std::string actionName,
                     llvm::json::Object defaults = llvm::json::Object());

        std::string name() const override { return actionName_; }

        llvm::Expected<ActionResult> run(
            const llvm::json::Object& params,
            TaskContext& ctx) const override;

    private:
        ResolvedPlugin plugin_;
        std::string actionName_;
        llvm::json::Object defaults_;
    };

    // Factory: returns one PluginAction per action the plugin
    // advertises in its sidecar (`details.plugin.actions`). Caller
    // registers each in the ActionRegistry. The plugin is captured
    // by-value so each PluginAction owns its own copy — Resolved
    // Plugin is cheap to copy and this avoids lifetime coupling
    // between the registry and whoever owns the resolved plugin
    // list.
    std::vector<std::unique_ptr<PluginAction>> makePluginActions(
        const ResolvedPlugin& plugin);

    // As above, threading the consumer's per-plugin `config` block into
    // every action as its default params.
    std::vector<std::unique_ptr<PluginAction>> makePluginActions(
        const ResolvedPlugin& plugin, const llvm::json::Object& config);

} // namespace cajeta::buildtool
