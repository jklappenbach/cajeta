#include "cajeta/buildtool/PluginAction.h"

#include "cajeta/buildtool/PluginRuntime.h"

namespace cajeta::buildtool {

    PluginAction::PluginAction(ResolvedPlugin plugin, std::string actionName,
                               llvm::json::Object defaults)
        : plugin_(std::move(plugin)), actionName_(std::move(actionName)),
          defaults_(std::move(defaults)) {}

    llvm::Expected<ActionResult> PluginAction::run(
        const llvm::json::Object& params,
        TaskContext& ctx) const {
        // Layering: the plugin's manifest config is the default layer;
        // the task's explicit action params overwrite key-by-key.
        llvm::json::Object merged = defaults_;
        for (const auto& kv : params) {
            merged[kv.first] = kv.second;
        }
        return invokePluginAction(plugin_, actionName_, merged, ctx);
    }

    std::vector<std::unique_ptr<PluginAction>> makePluginActions(
        const ResolvedPlugin& plugin) {
        return makePluginActions(plugin, llvm::json::Object());
    }

    std::vector<std::unique_ptr<PluginAction>> makePluginActions(
        const ResolvedPlugin& plugin, const llvm::json::Object& config) {
        std::vector<std::unique_ptr<PluginAction>> out;
        out.reserve(plugin.actionNames.size());
        for (const auto& an : plugin.actionNames) {
            llvm::json::Object defaults = config;
            out.push_back(std::make_unique<PluginAction>(plugin, an,
                                                         std::move(defaults)));
        }
        return out;
    }

} // namespace cajeta::buildtool
