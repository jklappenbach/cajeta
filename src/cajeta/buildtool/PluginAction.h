// Action subclass wrapping one plugin-provided action: run() forwards to
// PluginRuntime::invoke, so the TaskRunner never branches native-vs-plugin.

#pragma once

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Plugin.h"

#include <memory>
#include <string>

namespace cajeta::buildtool {

    class PluginAction : public Action {
    public:
        // `defaults` is the plugin manifest's `config` block — the default
        // parameter layer; explicit task params overlay it at run().
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

    // One PluginAction per action the plugin advertises in its sidecar
    // (`details.plugin.actions`); the plugin is captured by value.
    std::vector<std::unique_ptr<PluginAction>> makePluginActions(
        const ResolvedPlugin& plugin);

    // As above, with the consumer's per-plugin `config` block as default params.
    std::vector<std::unique_ptr<PluginAction>> makePluginActions(
        const ResolvedPlugin& plugin, const llvm::json::Object& config);

} // namespace cajeta::buildtool
