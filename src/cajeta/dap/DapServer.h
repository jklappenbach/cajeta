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

    // A DAP event envelope.
    Json makeEvent(int seq, const std::string& event, Json body);

    // The `stackTrace` response body for a stopped frame (CP4: one frame).
    // Resolves the StopEvent's loc id against the table for file + line; the
    // frame name is the loc's recorded function.
    Json stackTraceBody(const cajeta::dbg::StopEvent& stop,
                        const cajeta::dbg::DbgLocTable& table);

    // One DAP `variables` entry for a local (CP7-1d). `renderedValue` is the
    // already-formatted value (so this builder stays pure — no memory deref —
    // and unit-tests without a live address). Beyond {name, type, value,
    // variablesReference} it carries the CP7 memory facets two ways: a
    // namespaced `cajeta` sub-object with the textual alloc/ownership/lifetime
    // tags (the authoritative, color-independent carrier, FR-3.1/FR-5.3), and a
    // DAP-standard `presentationHint.attributes:["readOnly"]` for a moved-out
    // binding so a generic client also blocks editing a consumed value (FR-4.3).
    Json variableJson(const cajeta::dbg::DbgVar& v,
                      const std::string& renderedValue);

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

        // The `cajeta dap` entry (resident-debug-server Unit 8): run over
        // REAL stdio with the debuggee's stdout isolated from the protocol.
        // The JIT'd program writes fd 1 — the same fd the frames use — and a
        // print landing mid-frame corrupts the channel (observed: tour's
        // self-check output desyncs the client). POSIX: the protocol moves
        // to a private dup of stdout, fd 1 becomes a pipe pumped back as
        // `output` events (category "stdout"). Elsewhere: plain run().
        int runOverStdio();

        // Identity handshake test seams (resident-debug-server 5.1.1):
        // pretend the startup snapshot was taken before a rebuild, and
        // expose the resolved self-exe path so a test can send a MATCHING
        // compilerPath.
        void overrideSelfIdentityForTest(std::string identity) {
            selfIdentityAtStart_ = std::move(identity);
        }
        static std::string selfExePathForTest();

    private:
        // Compiler-identity handshake (resident-debug-server 5.2.1): refuse
        // the session and end the loop when the running image no longer
        // matches its on-disk binary, or when the client expects a different
        // binary (`compilerPath` on initialize). Returns true when the
        // session may proceed.
        bool verifyCompilerIdentity(const Json& args, const Emit& emit,
                                    int requestSeq);
        std::string selfIdentityAtStart_;   // "" until the first initialize
        // Serializes frame writes between the request loop and the debuggee-
        // stdout pump (Unit 8) — a frame is atomic on the wire or the client
        // desyncs. Also guards seq_ for pump-emitted events.
        std::mutex emitMutex_;

        // Drive the running program until it next stops at a breakpoint or
        // terminates; emit the matching `stopped` / `terminated` event.
        void runToStopOrExit(const Emit& emit);

        // Whether the program should actually park at this safepoint: true if
        // the matching breakpoint has no condition, or its condition holds
        // against the stopped frame's locals (CP6f). A false condition is
        // silently resumed in runToStopOrExit.
        bool shouldStopAt(const cajeta::dbg::StopEvent& stop,
                          const std::vector<cajeta::dbg::DbgFrameInfo>& frames) const;

        // CP6f-2b-ii: at each stop, build a flat frame table across ALL
        // threads/fibers (the stopped thread's chain + every other live fiber's
        // chain) and reset the variablesReference handle table. `stoppedFrames`
        // is the already-walked chain of the stopped thread (avoids re-walking).
        void rebuildFrameTable(std::vector<cajeta::dbg::DbgFrameInfo> stoppedFrames);

        // One decoded stack frame plus the thread/fiber it belongs to.
        struct FrameEntry {
            int threadId;                    // owning thread (0=entry) / fiber id
            cajeta::dbg::DbgFrameInfo info;  // func, locId, locals
        };

        int seq_ = 1;                          // outbound seq counter
        cajeta::jit::JitRunOptions launchOpts_;
        // DAP launch `stopOnEntry`. The plugin has always sent this; until now
        // nothing read it, so the IDE checkbox did nothing.
        bool stopOnEntry_ = false;
        // The launch environment overlay, and whether the shell's environment
        // is inherited under it (spec §4). Applied at configurationDone.
        std::map<std::string, std::string> launchEnv_;
        bool inheritSystemEnv_ = true;
        // Undoes that overlay. The JIT runs IN-PROCESS, so applying the
        // environment mutates this server; restoring on destruction is what
        // keeps one session out of the next (spec 4.1.4) even when the session
        // never ends cleanly.
        cajeta::util::EnvironmentScope envScope_;
        std::vector<cajeta::jit::Breakpoint> breakpoints_;
        // CP6f: per-breakpoint condition keyed by (file basename, line). Empty
        // or absent entry means an unconditional breakpoint.
        std::map<std::pair<std::string, int>, std::string> conditions_;
        // CP6f-3: desired break-on-throw state from setExceptionBreakpoints.
        // Recorded pre-launch; applied to the controller in configurationDone.
        bool exceptionsArmed_ = false;
        std::unique_ptr<cajeta::jit::JitDebugSession> session_;
        cajeta::dbg::StopEvent currentStop_;   // last stop (for stackTrace)
        // CP6f-2b-ii: flat per-stop frame table across all threads/fibers. The
        // DAP frameId is a monotonic index into this (no per-thread arithmetic);
        // stackTrace slices it by threadId. Rebuilt on each stop, cleared on
        // termination.
        std::vector<FrameEntry> frameTable_;
        // CP6f-2b-ii: opaque variablesReference handle table. DAP reserves ref 0
        // for "no children", so handles count up from 1 — no `frameId+1` trick.
        // Each maps to a frameTable_ index (that frame's Locals scope).
        std::map<int, int> varRefToFrame_;
        // variable-inspection Unit 4: aggregate-expansion handles share the same
        // ref space as varRefToFrame_ (nextVarRef_ hands out both). Each names an
        // aggregate to drill into — its canonical type, the slot address, and the
        // page offset to resume element enumeration from (0 for the first page; a
        // "more" node stores the next offset). Minted on `variables`, cleared on
        // resume/terminate alongside frameTable_.
        struct AggregateRef {
            std::string typeName;
            void* addr = nullptr;
            size_t start = 0;
        };
        std::map<int, AggregateRef> varRefToAggregate_;
        int nextVarRef_ = 1;
        // Elements returned per page when expanding an array (launch `pageSize`;
        // a missing or non-positive value falls back to this default).
        size_t pageSize_ = 100;

        // Mint a fresh variablesReference for an aggregate at (type, addr, start).
        int mintAggregateRef(const std::string& type, void* addr,
                             size_t start = 0);
        bool haveStop_ = false;
        bool terminated_ = false;
        int exitCode_ = 0;
    };

} // namespace cajeta::dap
