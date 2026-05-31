//
// DAP server (CP4): drives a JitDebugSession over the Debug Adapter Protocol.
//
// Implements the minimal request set for the slice:
//   initialize, launch, setBreakpoints, configurationDone, threads,
//   stackTrace, continue, disconnect
// plus the `initialized`, `stopped`, and `terminated` events.
//
// Design for testability: handle() processes ONE request and emits the
// response + any events through a callback, so a test can drive a scripted
// session deterministically without real I/O or threads at the test level.
// run() wraps handle() in a Content-Length framed read/write loop over streams
// (the production stdio path). `cajeta dap` calls run(cin, cout).
//
// CP4 scope: stackTrace returns the single stopped frame (file/line/function)
// derived from the StopEvent + DbgLocTable. Multi-frame stacks with locals
// arrive in CP5 (the per-fiber dbg_top chain).
//
#pragma once

#include <functional>
#include <istream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "cajeta/dap/Json.h"
#include "cajeta/dbg/DebugController.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/dbg/DebugVars.h"
#include "cajeta/jit/CajetaJitHost.h"

namespace cajeta::dap {

    // --- pure message builders (unit-tested directly) ---

    // A DAP response envelope for `command` request #requestSeq.
    Json makeResponse(int seq, int requestSeq, const std::string& command,
                      bool success, Json body);

    // A DAP event envelope.
    Json makeEvent(int seq, const std::string& event, Json body);

    // The `stackTrace` response body for a stopped frame (CP4: one frame).
    // Resolves the StopEvent's loc id against the table for file + line; the
    // frame name is the loc's recorded function.
    Json stackTraceBody(const cajeta::dbg::StopEvent& stop,
                        const cajeta::dbg::DbgLocTable& table);

    class DapServer {
    public:
        using Emit = std::function<void(const Json&)>;

        DapServer();
        ~DapServer();

        // Process one request; emit the response and any events via `emit`.
        // Returns false once the session should end (disconnect/terminate).
        bool handle(const Json& request, const Emit& emit);

        // Production loop: read framed requests from `in`, write framed
        // responses/events to `out`, until disconnect or EOF. Returns the
        // debuggee's exit code (0 if it never launched).
        int run(std::istream& in, std::ostream& out);

    private:
        // Drive the running program until it next stops at a breakpoint or
        // terminates; emit the matching `stopped` / `terminated` event.
        void runToStopOrExit(const Emit& emit);

        // Whether the program should actually park at this safepoint: true if
        // the matching breakpoint has no condition, or its condition holds
        // against the stopped frame's locals (CP6f). A false condition is
        // silently resumed in runToStopOrExit.
        bool shouldStopAt(const cajeta::dbg::StopEvent& stop,
                          const std::vector<cajeta::dbg::DbgFrameInfo>& frames) const;

        int seq_ = 1;                          // outbound seq counter
        cajeta::jit::JitRunOptions launchOpts_;
        std::vector<cajeta::jit::Breakpoint> breakpoints_;
        // CP6f: per-breakpoint condition keyed by (file basename, line). Empty
        // or absent entry means an unconditional breakpoint.
        std::map<std::pair<std::string, int>, std::string> conditions_;
        std::unique_ptr<cajeta::jit::JitDebugSession> session_;
        cajeta::dbg::StopEvent currentStop_;   // last stop (for stackTrace)
        // CP5: frames + locals snapshotted from the dbg chain at the last stop
        // (innermost first). Valid until the next resume; rebuilt on each stop.
        // frame N's "Locals" scope uses variablesReference N+1.
        std::vector<cajeta::dbg::DbgFrameInfo> frames_;
        bool haveStop_ = false;
        bool terminated_ = false;
        int exitCode_ = 0;
    };

} // namespace cajeta::dap
