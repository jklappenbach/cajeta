#include "cajeta/buildtool/PluginAction.h"

#include "cajeta/buildtool/PluginRuntime.h"

namespace cajeta::buildtool {

    PluginAction::PluginAction(ResolvedPlugin plugin, std::string actionName)
        : plugin_(std::move(plugin)), actionName_(std::move(actionName)) {}

    llvm::Expected<ActionResult> PluginAction::run(
        const llvm::json::Object& params,
        TaskContext& ctx) const {
        return invokePluginAction(plugin_, actionName_, params, ctx);
    }

    std::vector<std::unique_ptr<PluginAction>> makePluginActions(
        const ResolvedPlugin& plugin) {
        std::vector<std::unique_ptr<PluginAction>> out;
        out.reserve(plugin.actionNames.size());
        for (const auto& an : plugin.actionNames) {
            out.push_back(std::make_unique<PluginAction>(plugin, an));
        }
        return out;
    }

} // namespace cajeta::buildtool
