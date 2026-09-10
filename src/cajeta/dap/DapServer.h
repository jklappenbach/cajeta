// DAP server: drives a JitDebugSession over the Debug Adapter Protocol.
// handle() takes ONE request and emits its response and events through a
// callback, so a scripted session tests without I/O; run() frames it on streams.
#pragma once

#include <functional>
#include <istream>
#include <map>
#include <mutex>
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
#include "cajeta/util/Environment.h"

namespace cajeta::dap {

    // --- pure message builders (unit-tested directly) ---

    // A DAP response envelope for `command` request #requestSeq.
    Json makeResponse(int seq, int requestSeq, const std::string& command,
                      bool success, Json body);

    Json makeEvent(int seq, const std::string& event, Json body);

    // The `stackTrace` body for a stopped frame, its loc id resolved via `table`.
    Json stackTraceBody(const cajeta::dbg::StopEvent& stop,
                        const cajeta::dbg::DbgLocTable& table);

    // One DAP `variables` entry, from an already-formatted `renderedValue` so
    // this stays pure; memory facets ride a `cajeta` sub-object and a hint.
    Json variableJson(const cajeta::dbg::DbgVar& v,
                      const std::string& renderedValue);

    class DapServer {
    public:
        using Emit = std::function<void(const Json&)>;

        DapServer();
        ~DapServer();

        // Processes one request, emitting its response and events via `emit`.
        // Always true: disconnect ends the session, but the loop ends at EOF.
        bool handle(const Json& request, const Emit& emit);

        // Frames requests off `in` onto `out` until EOF; the debuggee's exit code.
        int run(std::istream& in, std::ostream& out);

        // run() over REAL stdio. The JIT'd program writes fd 1 too and a print
        // mid-frame corrupts the channel, so on POSIX the protocol moves to a
        // private dup and fd 1 becomes a pipe pumped back as `output` events.
        int runOverStdio();

        // Test seam: pretend the startup snapshot predates a rebuild.
        void overrideSelfIdentityForTest(std::string identity) {
            selfIdentityAtStart_ = std::move(identity);
        }
        static std::string selfExePathForTest();

    private:
        // False — ending the session — when the image is not the expected one.
        bool verifyCompilerIdentity(const Json& args, const Emit& emit,
                                    int requestSeq);
        std::string selfIdentityAtStart_;   // "" until the first initialize
        // A frame is atomic on the wire; this also guards seq_.
        std::mutex emitMutex_;

        // Disarms every stop source and resumes before joining: a plain join
        // hangs forever on a PARKED program.
        void drainToExit();

        // Runs to the next stop or termination, emitting the matching event.
        void runToStopOrExit(const Emit& emit);

        // Whether to park here: unconditional, or the condition holds on `frames`.
        bool shouldStopAt(const cajeta::dbg::StopEvent& stop,
                          const std::vector<cajeta::dbg::DbgFrameInfo>& frames) const;

        // Rebuilds the all-fiber frame table from the already-walked chain.
        void rebuildFrameTable(std::vector<cajeta::dbg::DbgFrameInfo> stoppedFrames);

        // One decoded stack frame plus the thread/fiber it belongs to.
        struct FrameEntry {
            int threadId;                    // owning thread (0=entry) / fiber id
            cajeta::dbg::DbgFrameInfo info;  // func, locId, locals
        };

        int seq_ = 1;                          // outbound seq counter
        cajeta::jit::JitRunOptions launchOpts_;
        // DAP launch `stopOnEntry`.
        bool stopOnEntry_ = false;
        // The launch environment overlay, applied at configurationDone.
        std::map<std::string, std::string> launchEnv_;
        bool inheritSystemEnv_ = true;
        // Undoes it: the JIT is IN-PROCESS, so this keeps sessions apart.
        cajeta::util::EnvironmentScope envScope_;
        std::vector<cajeta::jit::Breakpoint> breakpoints_;
        // Ids for breakpoints_, parallel by index: setBreakpoints answers
        // `verified` pre-compile, so configurationDone downgrades by id later.
        std::vector<int> breakpointIds_;
        int nextBreakpointId_ = 1;
        // Conditions keyed by (file basename, line); absent = unconditional.
        std::map<std::pair<std::string, int>, std::string> conditions_;
        // Break-on-throw, recorded pre-launch and applied at configurationDone.
        bool exceptionsArmed_ = false;
        std::unique_ptr<cajeta::jit::JitDebugSession> session_;
        cajeta::dbg::StopEvent currentStop_;   // last stop (for stackTrace)
        // Every fiber's frames, flat: a DAP frameId is an index into this.
        std::vector<FrameEntry> frameTable_;
        // Handles to frameTable_ indices, counting from 1 since DAP reserves 0.
        std::map<int, int> varRefToFrame_;
        // Aggregate-expansion handles in that same ref space; `start` resumes.
        struct AggregateRef {
            std::string typeName;
            void* addr = nullptr;
            size_t start = 0;
        };
        std::map<int, AggregateRef> varRefToAggregate_;
        int nextVarRef_ = 1;
        // Elements per page when expanding an array (launch `pageSize`).
        size_t pageSize_ = 100;

        // Mint a fresh variablesReference for an aggregate at (type, addr, start).
        int mintAggregateRef(const std::string& type, void* addr,
                             size_t start = 0);
        bool haveStop_ = false;
        bool terminated_ = false;
        int exitCode_ = 0;
    };

} // namespace cajeta::dap
