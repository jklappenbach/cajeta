// Plugin subprocess runtime — spawns a plugin binary, sends the action request
// as JSON on stdin, reads JSON-line records back from stdout and aggregates
// them into an ActionResult. Protocol: PluginRuntime.cpp's namespace block.

#pragma once

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Plugin.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <string>

namespace cajeta::buildtool {

    // Dispatch one invocation of the plugin-provided `actionName`, with
    // already-substituted `params`. A plugin reporting a logical error still
    // succeeds; llvm::Error means the call itself faulted.
    llvm::Expected<ActionResult> invokePluginAction(
        const ResolvedPlugin& plugin,
        const std::string& actionName,
        const llvm::json::Object& params,
        TaskContext& ctx);

} // namespace cajeta::buildtool
