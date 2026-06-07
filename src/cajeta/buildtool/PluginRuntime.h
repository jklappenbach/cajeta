// Plugin subprocess runtime — spawns a plugin binary, sends the
// action request as JSON on stdin, reads JSON-line records back from
// stdout, aggregates them into an ActionResult.
//
// The protocol is documented in PluginRuntime.cpp's namespace block —
// keep that as the canonical spec. The same protocol is what the
// future in-process (LLJIT) runtime will speak across function-call
// boundaries instead of pipes; this file is the v1 subprocess host.

#pragma once

#include "cajeta/buildtool/Action.h"
#include "cajeta/buildtool/Plugin.h"

#include <llvm/Support/Error.h>
#include <llvm/Support/JSON.h>

#include <string>

namespace cajeta::buildtool {

    // Dispatch one invocation of a plugin-provided action.
    //
    //   plugin       — the resolved plugin owning `actionName`. Must
    //                  carry a non-empty `binaryPath` (the resolver
    //                  records it from `details.plugin.binary` in the
    //                  plugin's sidecar manifest).
    //   actionName   — the namespaced action name as it appeared in
    //                  the task (e.g. `"cajeta.coverage.report"`).
    //                  Echoed back to the plugin so it knows which
    //                  entry to call when it ships more than one.
    //   params       — substituted action params (the `${id.field}`
    //                  expansion already done by the TaskRunner).
    //                  Passed verbatim to the plugin via stdin.
    //   ctx          — the task context. The runtime reads workdir +
    //                  project identity from it for the request, and
    //                  forwards plugin writes/warns back through it.
    //
    // Returns the action result on protocol success — that includes
    // the plugin reporting a logical error (the ActionResult carries
    // an error message but the call itself didn't fault). Returns an
    // llvm::Error only on infrastructure failures: missing binary,
    // exec failure, plugin crash, malformed protocol records.
    llvm::Expected<ActionResult> invokePluginAction(
        const ResolvedPlugin& plugin,
        const std::string& actionName,
        const llvm::json::Object& params,
        TaskContext& ctx);

} // namespace cajeta::buildtool
