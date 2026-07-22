#include "cajeta/dap/DapServer.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>
#ifndef _WIN32
#include <ext/stdio_filebuf.h>
#include <unistd.h>
#endif

#include "cajeta/dap/DapProtocol.h"
#include "cajeta/dbg/DebugLocTable.h"

namespace cajeta::dap {

using cajeta::dbg::StopEvent;
using cajeta::dbg::DbgLocTable;
using cajeta::dbg::globalDbgLocTable;

Json makeResponse(int seq, int requestSeq, const std::string& command,
                  bool success, Json body) {
    Json r = Json::object();
    r["seq"] = seq;
    r["type"] = "response";
    r["request_seq"] = requestSeq;
    r["command"] = command;
    r["success"] = success;
    r["body"] = std::move(body);
    return r;
}

Json makeEvent(int seq, const std::string& event, Json body) {
    Json e = Json::object();
    e["seq"] = seq;
    e["type"] = "event";
    e["event"] = event;
    e["body"] = std::move(body);
    return e;
}

Json stackTraceBody(const StopEvent& stop, const DbgLocTable& table) {
    Json frames = Json::array();
    if (stop.locId >= 0 && static_cast<size_t>(stop.locId) < table.size()) {
        const auto& loc = table.at(stop.locId);
        Json frame = Json::object();
        frame["id"] = 0;
        frame["name"] = loc.function.empty() ? std::string("<entry>")
                                             : loc.function;
        frame["line"] = loc.line;
        frame["column"] = loc.col > 0 ? loc.col : 1;
        Json source = Json::object();
        source["name"] = std::filesystem::path(loc.file).filename().string();
        source["path"] = loc.file;
        frame["source"] = std::move(source);
        frames.push_back(std::move(frame));
    }
    Json body = Json::object();
    body["stackFrames"] = std::move(frames);
    body["totalFrames"] = static_cast<int>(
        body.at("stackFrames").size());
    return body;
}

Json variableJson(const cajeta::dbg::DbgVar& v, const std::string& renderedValue) {
    using cajeta::dbg::LifetimeState;
    Json var = Json::object();
    var["name"] = v.name;
    var["value"] = renderedValue;
    var["type"] = v.type;
    var["variablesReference"] = 0;

    // Namespaced facet tags — the plugin's source of truth for the
    // icon/color/strike rendering, and a textual affordance on their own.
    Json meta = Json::object();
    meta["alloc"] = cajeta::dbg::allocClassName(v.alloc);
    meta["ownership"] = cajeta::dbg::ownershipRoleName(v.ownership);
    meta["lifetime"] = cajeta::dbg::lifetimeStateName(v.lifetime);
    var["cajeta"] = std::move(meta);

    // A moved-out binding is consumed: reading it is a language error, so it
    // must not look editable. Map to the standard read-only attribute.
    if (v.lifetime == LifetimeState::MovedOut) {
        Json attrs = Json::array();
        attrs.push_back(std::string("readOnly"));
        Json hint = Json::object();
        hint["attributes"] = std::move(attrs);
        var["presentationHint"] = std::move(hint);
    }
    return var;
}

// dap-stepping: chain-length accessor from the C runtime — a pure pointer
// chase, cheap enough to run per candidate safepoint while a step is pending
// (walkFrames would decode every local of every frame).
extern "C" int __cajeta_dbg_frame_depth(void* top);
// 9.1: chain-containment probe (same pure pointer chase family).
extern "C" int __cajeta_dbg_frame_contains(void* top, void* node);

namespace {
// dap-stepping: the controller compares the step origin's "line" as an opaque
// key. Fold the file into it so a coincidental line-number match in another
// source file doesn't read as "same line" (the same-line skip is per
// (file, line)). -1 = unknown loc; the masked hash can never produce it.
int lineKeyForLoc(int32_t locId) {
    const auto& table = globalDbgLocTable();
    if (locId < 0 || static_cast<size_t>(locId) >= table.size()) return -1;
    const auto& loc = table.at(locId);
    size_t h = std::hash<std::string>{}(loc.file) * 31u
             + static_cast<size_t>(loc.line);
    return static_cast<int>(h & 0x7fffffff);
}
} // namespace

namespace {
// The running server's own executable path (Linux: /proc/self/exe). Empty
// when unresolvable — identity checks then never refuse (fail-open: a
// missing /proc must not brick debugging on exotic hosts).
std::string selfExePath() {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string() : p.string();
}

// size:mtime of the on-disk binary — cheap, and a rebuild always changes it.
std::string diskIdentity(const std::string& path) {
    if (path.empty()) return {};
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) return {};
    auto mtime = std::filesystem::last_write_time(path, ec);
    if (ec) return {};
    return std::to_string((unsigned long long) size) + ":"
         + std::to_string(
               (long long) mtime.time_since_epoch().count());
}
} // namespace

std::string DapServer::selfExePathForTest() { return selfExePath(); }

bool DapServer::verifyCompilerIdentity(const Json& args, const Emit& emit,
                                       int requestSeq) {
    const std::string exe = selfExePath();
    const std::string now = diskIdentity(exe);
    auto refuse = [&](const std::string& why) {
        Json body = Json::object();
        body["category"] = "console";
        body["output"] = "cajeta: " + why + "; restarting debug server\n";
        emit(makeEvent(seq_++, "output", std::move(body)));
        emit(makeResponse(seq_++, requestSeq, "initialize", false, Json(why)));
        return false;
    };
    // Self-check: the on-disk binary changed under this running image
    // (a compiler rebuild). First initialize snapshots; later ones compare.
    if (!now.empty()) {
        if (selfIdentityAtStart_.empty()) selfIdentityAtStart_ = now;
        else if (selfIdentityAtStart_ != now)
            return refuse("compiler binary changed on disk");
    }
    // Client expectation: the binary the plugin LAUNCHED (or now intends).
    // A different path means the compilerPath setting moved — this server
    // is the wrong compiler entirely.
    const std::string expected = args.at("compilerPath").asString();
    if (!expected.empty() && !exe.empty()) {
        std::error_code ec;
        if (!std::filesystem::equivalent(expected, exe, ec) || ec)
            return refuse("debug server is not the configured compiler");
    }
    return true;
}

DapServer::DapServer() = default;

DapServer::~DapServer() {
    if (session_) session_->join();
}

void DapServer::runToStopOrExit(const Emit& emit) {
    if (!session_) return;
    using namespace std::chrono;
    // Poll: the program either hits an armed safepoint (controller parks ->
    // waitForStop returns) or runs to completion (isFinished). Short timeout
    // so we notice termination promptly.
    while (true) {
        StopEvent ev;
        if (session_->controller().waitForStop(ev, milliseconds(50))) {
            // CP5: snapshot the frame chain + locals while the carrier is
            // parked (the chain is stable until we resume).
            auto frames = cajeta::dbg::walkFrames(ev.frameTop);
            // CP6f: a conditional breakpoint whose condition is false does not
            // stop — resume the carrier and keep running. The snapshot above is
            // exactly what the condition needs (this frame's locals).
            if (!shouldStopAt(ev, frames)) {
                session_->controller().resume();
                continue;
            }
            currentStop_ = ev;
            haveStop_ = true;
            // CP6f-2b-ii: build the cross-thread frame table for this stop.
            rebuildFrameTable(std::move(frames));
            Json body = Json::object();
            // CP6f-3: reason reflects breakpoint vs exception vs step stop.
            // Step-decision trace: on a step stop, explain the landing in
            // stderr (file:line, depth vs origin, verdict per candidate).
            if (ev.reason == cajeta::dbg::StopEvent::StopReason::Step) {
                // The stopped chain itself: length + innermost functions. A
                // depth=1 verdict beside a deep walk = detached chain.
                {
                    auto walked = cajeta::dbg::walkFrames(ev.frameTop);
                    std::cerr << "[step-trace] STOP chain len=" << walked.size();
                    for (size_t i = 0; i < walked.size() && i < 3; ++i)
                        std::cerr << " [" << i << "]=" << walked[i].func;
                    std::cerr << "\n";
                }
                const auto& table = globalDbgLocTable();
                for (const auto& d : session_->controller().drainStepTrace()) {
                    const auto& loc = table.at(d.locId);
                    std::cerr << "[step-trace] loc=" << d.locId << " "
                              << loc.file << ":" << loc.line
                              << " fiber=" << d.fiberId
                              << " depth=" << d.depth
                              << " origin=" << d.originDepth
                              << " kind=" << d.kind
                              << (d.stopped ? "  << STOP" : "") << "\n";
                }
            }
            switch (ev.reason) {
                case cajeta::dbg::StopEvent::StopReason::Exception:
                    body["reason"] = "exception"; break;
                case cajeta::dbg::StopEvent::StopReason::Entry:
                    body["reason"] = "entry"; break;
                case cajeta::dbg::StopEvent::StopReason::Step:
                    body["reason"] = "step"; break;
                default:
                    body["reason"] = "breakpoint"; break;
            }
            // CP6f-2b: the real stopped fiber id (0 = entry/main thread, >=1 a
            // spawned fiber) instead of a hard-coded 1.
            body["threadId"] = static_cast<int>(ev.fiberId);
            // CP6f-2d: only claim a full stop-the-world when the quiesce barrier
            // confirmed every carrier parked. If some carrier was stuck (e.g. in
            // a native call) it stays running, so allThreadsStopped is false and
            // the client knows not every fiber's state is settled.
            body["allThreadsStopped"] = (ev.unquiescedCarriers == 0);
            emit(makeEvent(seq_++, "stopped", std::move(body)));
            return;
        }
        if (session_->isFinished()) {
            exitCode_ = session_->join();
            terminated_ = true;
            haveStop_ = false;
            frameTable_.clear();
            varRefToFrame_.clear();
            Json body = Json::object();
            body["exitCode"] = exitCode_;
            emit(makeEvent(seq_++, "exited", body));
            emit(makeEvent(seq_++, "terminated", Json::object()));
            return;
        }
    }
}

bool DapServer::shouldStopAt(const StopEvent& stop,
                             const std::vector<cajeta::dbg::DbgFrameInfo>& frames)
                             const {
    if (conditions_.empty()) return true;
    // Resolve the stopped loc to (file basename, line) and look up a condition.
    const auto& table = globalDbgLocTable();
    if (stop.locId < 0 || static_cast<size_t>(stop.locId) >= table.size())
        return true;
    const auto& loc = table.at(stop.locId);
    std::string base = std::filesystem::path(loc.file).filename().string();
    auto it = conditions_.find({base, loc.line});
    if (it == conditions_.end() || it->second.empty()) return true;
    // Evaluate against the innermost frame's locals (where the bp sits).
    if (frames.empty()) return true;
    std::string err;
    return cajeta::dbg::evaluateCondition(it->second, frames.front().locals,
                                          &err);
}

void DapServer::rebuildFrameTable(
        std::vector<cajeta::dbg::DbgFrameInfo> stoppedFrames) {
    frameTable_.clear();
    varRefToFrame_.clear();
    nextVarRef_ = 1;
    const int stoppedTid = static_cast<int>(currentStop_.fiberId);

    if (!stoppedFrames.empty()) {
        for (auto& fr : stoppedFrames)
            frameTable_.push_back(FrameEntry{stoppedTid, std::move(fr)});
    } else if (currentStop_.locId >= 0) {
        // CP4 fallback: no frame chain (a debug build without the CP5 frame
        // codegen). Synthesize a single frame for the stopped thread from the
        // loc table so stackTrace still shows where we are.
        const auto& table = globalDbgLocTable();
        cajeta::dbg::DbgFrameInfo fr;
        if (static_cast<size_t>(currentStop_.locId) < table.size())
            fr.func = table.at(currentStop_.locId).function;
        fr.locId = currentStop_.locId;
        frameTable_.push_back(FrameEntry{stoppedTid, std::move(fr)});
    }

    // Every other live fiber's chain. The carrier is parked at the stopped
    // safepoint, so these chains are stable to walk (CP6f-2b). The stopped
    // fiber is already in the registry; skip it to avoid a duplicate.
    if (session_) {
        for (const auto& f : session_->liveFibers()) {
            if (f.id == stoppedTid) continue;
            for (auto& fr : cajeta::dbg::walkFrames(f.frameTop))
                frameTable_.push_back(FrameEntry{f.id, std::move(fr)});
        }
    }
}

bool DapServer::handle(const Json& request, const Emit& emit) {
    const std::string command = request.at("command").asString();
    const int requestSeq = request.at("seq").asInt();
    const Json& args = request.at("arguments");

    if (command == "initialize") {
        if (!verifyCompilerIdentity(args, emit, requestSeq))
            return false;   // clean refusal; the launcher respawns fresh

        Json caps = Json::object();
        caps["supportsConfigurationDoneRequest"] = true;
        caps["supportsSetVariable"] = true;
        // CP6f-3: advertise an "all throws" exception filter so the client can
        // offer break-on-throw. Single filter for now (no type filtering yet).
        Json filter = Json::object();
        filter["filter"] = "all";
        filter["label"] = "All thrown exceptions";
        filter["default"] = false;
        Json filters = Json::array();
        filters.push_back(std::move(filter));
        caps["exceptionBreakpointFilters"] = std::move(filters);
        emit(makeResponse(seq_++, requestSeq, command, true, caps));
        // Tell the client we're ready for breakpoint configuration.
        emit(makeEvent(seq_++, "initialized", Json::object()));
        return true;
    }

    if (command == "launch") {
        // Accept either the documented "entry-method" or "entryMethod".
        std::string entry = args.at("entry-method").asString();
        if (entry.empty()) entry = args.at("entryMethod").asString();
        // Dotted form is what runJit wants; the doc uses "Class::method" too —
        // normalize a single "::" to ".".
        auto pos = entry.find("::");
        if (pos != std::string::npos) entry = entry.substr(0, pos) + "."
                                            + entry.substr(pos + 2);
        launchOpts_.entryMethod = entry;
        launchOpts_.sourceRoot = args.at("sourceRoot").asString();
        if (launchOpts_.sourceRoot.empty())
            launchOpts_.sourceRoot = args.at("source-root").asString();
        // fast-debug-launch 5.2.1: whole-program cache root. Absence (or
        // empty) = unspecified = full compile — the same convention as `env`,
        // so every pre-cache client keeps today's behavior.
        launchOpts_.cacheDir = args.at("cacheDir").asString();
        if (launchOpts_.cacheDir.empty())
            launchOpts_.cacheDir = args.at("cache-dir").asString();
        // resident-debug-server 4.2.1: reuse the primed stdlib world across
        // this server's sessions. Absence = off (one-shot behavior).
        launchOpts_.resident = args.at("resident").isBool()
                                   ? args.at("resident").asBool()
                                   : false;
        stopOnEntry_ = args.at("stopOnEntry").asBool();
        // Environment (spec §4). Absence of "env" means UNSPECIFIED, not
        // "empty environment" — every launch sent before this feature existed
        // omits it, and reading that as an empty environment would blank the
        // debuggee's. Same for the inherit flag, which defaults to on.
        launchEnv_.clear();
        const Json& env = args.at("env");
        if (env.isObject())
            for (const auto& kv : env.items())
                launchEnv_[kv.first] = kv.second.asString();
        inheritSystemEnv_ = args.at("inheritSystemEnv").isBool()
                                ? args.at("inheritSystemEnv").asBool()
                                : true;
        emit(makeResponse(seq_++, requestSeq, command, true, Json::object()));
        return true;
    }

    if (command == "setBreakpoints") {
        // args.source.path + args.breakpoints[].line
        std::string path = args.at("source").at("path").asString();
        if (path.empty()) path = args.at("source").at("name").asString();
        std::string base = std::filesystem::path(path).filename().string();
        Json verified = Json::array();
        const Json& bps = args.at("breakpoints");
        for (size_t i = 0; i < bps.size(); ++i) {
            int line = bps[i].at("line").asInt();
            breakpoints_.push_back(cajeta::jit::Breakpoint{base, line});
            // CP6f: an optional condition (whole-file replace semantics — a
            // bp without a condition clears any prior one for that loc).
            std::string cond = bps[i].at("condition").asString();
            if (!cond.empty()) conditions_[{base, line}] = cond;
            else conditions_.erase({base, line});
            Json b = Json::object();
            b["verified"] = true;
            b["line"] = line;
            verified.push_back(std::move(b));
        }
        Json body = Json::object();
        body["breakpoints"] = std::move(verified);
        emit(makeResponse(seq_++, requestSeq, command, true, std::move(body)));
        return true;
    }

    if (command == "setExceptionBreakpoints") {
        // CP6f-3: a non-empty `filters` array arms break-on-throw (single
        // all-throws toggle for now — type filtering is a later cut). Whole-
        // replace semantics: an empty array disarms. The desired state is
        // recorded and applied to the controller once the session exists
        // (configurationDone); if a session is already running, apply live.
        const Json& filters = args.at("filters");
        exceptionsArmed_ = filters.size() > 0;
        if (session_) {
            if (exceptionsArmed_) session_->controller().armException();
            else session_->controller().disarmException();
        }
        emit(makeResponse(seq_++, requestSeq, command, true, Json::object()));
        return true;
    }

    if (command == "configurationDone") {
        std::string err;
        // The launch environment is applied through startDebugSession's
        // beforeRun hook: after the build, before the program thread starts.
        // The debuggee's first System.env.get therefore sees it (spec 4.1.3)
        // while the in-process compile still runs under the real environment —
        // which matters because an inheritSystemEnv=false configuration
        // suppresses every undeclared variable, and the build needs PATH and
        // friends. envScope_ remembers what it displaced and restores from its
        // destructor (4.1.4).
        auto applyEnv = [this]() {
            if (!launchEnv_.empty() || !inheritSystemEnv_)
                envScope_.apply(launchEnv_, inheritSystemEnv_);
        };
        // fast-debug-launch 2.2.1: narrate the in-process compile as `output`
        // events so a working launch never reads as a hang. buildJit runs on
        // THIS thread inside startDebugSession, so emitting through `emit` is
        // safe and ordered; the callback is cleared right after because it
        // captures the stack-scoped `emit`.
        launchOpts_.onProgress = [this, &emit](const std::string& phase,
                                               const std::string& detail,
                                               int current, int total) {
            std::string line;
            if (phase == "collect") line = "cajeta: compile started\n";
            else if (phase == "parse" && detail == "resident-world")
                line = "cajeta: resident world reused\n";
            else if (phase == "parse" && total > 0)
                line = "cajeta: compiling [" + std::to_string(current) + "/"
                     + std::to_string(total) + "] " + detail + "\n";
            else if (phase == "codegen") line = "cajeta: generating code\n";
            else if (phase == "merge") line = "cajeta: linking modules\n";
            else if (phase == "jit")
                line = detail == "cached" ? "cajeta: using cached build\n"
                                          : "cajeta: preparing JIT\n";
            if (line.empty()) return;
            Json body = Json::object();
            body["category"] = "console";
            body["output"] = std::move(line);
            emit(makeEvent(seq_++, "output", std::move(body)));
        };
        // CP6f-3: arm break-on-throw inside startDebugSession (before the
        // program thread starts) so an immediate throw can't race past it.
        session_ = cajeta::jit::startDebugSession(launchOpts_, breakpoints_,
                                                  &err, exceptionsArmed_,
                                                  stopOnEntry_, applyEnv);
        launchOpts_.onProgress = nullptr;
        bool ok = session_ != nullptr;
        if (ok) {
            Json body = Json::object();
            body["category"] = "console";
            body["output"] = "cajeta: compile finished\n";
            emit(makeEvent(seq_++, "output", std::move(body)));
            // dap-stepping: give the controller its depth/line seams. Depth is
            // the carrier's own frame-chain length at the safepoint (the
            // carrier is executing the chase, so the chain is stable); line is
            // the (file, line) key. Only consulted while a step is pending,
            // and depth only once the line already differs.
            session_->controller().setStepProviders(
                [](void* frameTop) { return __cajeta_dbg_frame_depth(frameTop); },
                [](int32_t locId) { return lineKeyForLoc(locId); },
                [](void* frameTop, void* origin) {
                    return __cajeta_dbg_frame_contains(frameTop, origin) != 0;
                });
        }
        emit(makeResponse(seq_++, requestSeq, command, ok,
                          ok ? Json::object() : Json(err)));
        if (ok) runToStopOrExit(emit);
        return true;
    }

    if (command == "threads") {
        // CP6f-2b: the program/entry thread is always id 0 ("main"); each live
        // fiber from the JIT registry is an additional thread keyed by its
        // stable dbg id.
        //
        // FIXME(CP6f-2d, specs/archive/carrier-quiesce-spec.md): this enumeration
        // is NOT yet safe under the multi-carrier scheduler. Only the carrier
        // that hit the breakpoint is parked (DebugController::onSafepoint blocks
        // one thread); the other __cajeta_carriers[] keep running fibers, so the
        // registry can be mutated (register/unregister) concurrently while we
        // walk it, and liveFibers() reads count() then at(i) with the registry
        // lock released between calls (a TOCTOU). Correct behavior needs
        // cross-carrier stop-the-world quiesce before inspection.
        Json threads = Json::array();
        Json main = Json::object();
        main["id"] = 0;
        main["name"] = "main";
        threads.push_back(std::move(main));
        if (session_) {
            for (const auto& f : session_->liveFibers()) {
                Json t = Json::object();
                t["id"] = f.id;
                t["name"] = "fiber " + std::to_string(f.id);
                threads.push_back(std::move(t));
            }
        }
        Json body = Json::object();
        body["threads"] = std::move(threads);
        emit(makeResponse(seq_++, requestSeq, command, true, std::move(body)));
        return true;
    }

    if (command == "stackTrace") {
        // CP6f-2b-ii: return the slice of the per-stop frame table belonging to
        // the requested thread. frameId is the GLOBAL monotonic index into the
        // table (stable for this stop, spanning all threads). A missing threadId
        // defaults to the stopped thread (the existing single-thread tests pass
        // none; the stopped thread there is id 0).
        const int stoppedTid = static_cast<int>(currentStop_.fiberId);
        const int threadId =
            args.has("threadId") ? args.at("threadId").asInt() : stoppedTid;
        const auto& table = globalDbgLocTable();
        Json frames = Json::array();
        if (haveStop_) {
            for (size_t i = 0; i < frameTable_.size(); ++i) {
                if (frameTable_[i].threadId != threadId) continue;
                const auto& fr = frameTable_[i].info;
                Json frame = Json::object();
                frame["id"] = static_cast<int>(i);   // global, monotonic
                frame["name"] = fr.func.empty() ? std::string("<entry>")
                                                : fr.func;
                int line = 0, col = 1;
                std::string file;
                if (fr.locId >= 0 &&
                        static_cast<size_t>(fr.locId) < table.size()) {
                    const auto& loc = table.at(fr.locId);
                    line = loc.line;
                    col = loc.col > 0 ? loc.col : 1;
                    file = loc.file;
                }
                frame["line"] = line;
                frame["column"] = col;
                if (!file.empty()) {
                    Json source = Json::object();
                    source["name"] =
                        std::filesystem::path(file).filename().string();
                    source["path"] = file;
                    frame["source"] = std::move(source);
                }
                frames.push_back(std::move(frame));
            }
        }
        Json body = Json::object();
        const int total = static_cast<int>(frames.size());
        body["stackFrames"] = std::move(frames);
        body["totalFrames"] = total;
        emit(makeResponse(seq_++, requestSeq, command, true, std::move(body)));
        return true;
    }

    if (command == "scopes") {
        // One "Locals" scope per frame. frameId is a global frameTable_ index;
        // we mint an opaque variablesReference handle (>=1) for it. The client
        // round-trips that handle to `variables`/`setVariable` — no arithmetic
        // relationship to frameId.
        int frameId = args.at("frameId").asInt();
        Json scopes = Json::array();
        if (frameId >= 0 && static_cast<size_t>(frameId) < frameTable_.size()) {
            int ref = nextVarRef_++;
            varRefToFrame_[ref] = frameId;
            Json scope = Json::object();
            scope["name"] = "Locals";
            scope["variablesReference"] = ref;
            scope["expensive"] = false;
            scopes.push_back(std::move(scope));
        }
        Json body = Json::object();
        body["scopes"] = std::move(scopes);
        emit(makeResponse(seq_++, requestSeq, command, true, std::move(body)));
        return true;
    }

    if (command == "variables") {
        int ref = args.at("variablesReference").asInt();
        Json vars = Json::array();
        auto it = varRefToFrame_.find(ref);
        if (it != varRefToFrame_.end() &&
                static_cast<size_t>(it->second) < frameTable_.size()) {
            for (const auto& v : frameTable_[it->second].info.locals) {
                vars.push_back(variableJson(
                    v, cajeta::dbg::formatValue(v.type, v.addr)));
            }
        }
        Json body = Json::object();
        body["variables"] = std::move(vars);
        emit(makeResponse(seq_++, requestSeq, command, true, std::move(body)));
        return true;
    }

    if (command == "setVariable") {
        int ref = args.at("variablesReference").asInt();
        std::string name = args.at("name").asString();
        std::string value = args.at("value").asString();
        bool ok = false;
        std::string err = "no such variable: " + name;
        std::string rendered;
        auto it = varRefToFrame_.find(ref);
        if (it != varRefToFrame_.end() &&
                static_cast<size_t>(it->second) < frameTable_.size()) {
            for (const auto& v : frameTable_[it->second].info.locals) {
                if (v.name != name) continue;
                ok = cajeta::dbg::writeValue(v.type, v.addr, value, &err);
                if (ok) rendered = cajeta::dbg::formatValue(v.type, v.addr);
                break;
            }
        }
        if (ok) {
            Json body = Json::object();
            body["value"] = rendered;
            emit(makeResponse(seq_++, requestSeq, command, true,
                              std::move(body)));
        } else {
            emit(makeResponse(seq_++, requestSeq, command, false, Json(err)));
        }
        return true;
    }

    if (command == "continue") {
        emit(makeResponse(seq_++, requestSeq, command, true, [] {
            Json b = Json::object();
            b["allThreadsContinued"] = true;
            return b;
        }()));
        if (session_ && !terminated_) {
            haveStop_ = false;
            session_->controller().resume();
            runToStopOrExit(emit);
        }
        return true;
    }

    if (command == "next" || command == "stepIn" || command == "stepOut") {
        // dap-stepping spec §3: only valid against the currently stopped
        // fiber. While running (or after termination) fail with a message —
        // never crash, never disturb the session.
        if (!session_ || terminated_ || !haveStop_) {
            emit(makeResponse(seq_++, requestSeq, command, false,
                              Json(std::string("cannot step: no stopped "
                                               "thread (program running or "
                                               "terminated)"))));
            return true;
        }
        const int stoppedTid = static_cast<int>(currentStop_.fiberId);
        const int threadId =
            args.has("threadId") ? args.at("threadId").asInt() : stoppedTid;
        if (threadId != stoppedTid) {
            emit(makeResponse(
                seq_++, requestSeq, command, false,
                Json("cannot step thread " + std::to_string(threadId) +
                     ": the stopped thread is " + std::to_string(stoppedTid))));
            return true;
        }
        // Origin: the stopped fiber's frame count and (file, line) key. An
        // exception stop has locId -1 — its line comes from the innermost
        // frame's recorded current loc.
        int originDepth = 0;
        int32_t originLoc = currentStop_.locId;
        for (const auto& fe : frameTable_) {
            if (fe.threadId != stoppedTid) continue;
            if (originDepth == 0 && originLoc < 0) originLoc = fe.info.locId;
            ++originDepth;
        }
        const cajeta::dbg::StepKind kind =
            command == "next"     ? cajeta::dbg::StepKind::Over
            : command == "stepIn" ? cajeta::dbg::StepKind::In
                                  : cajeta::dbg::StepKind::Out;
        emit(makeResponse(seq_++, requestSeq, command, true, Json::object()));
        haveStop_ = false;
        session_->controller().resumeWithStep(kind, currentStop_.fiberId,
                                              originDepth,
                                              lineKeyForLoc(originLoc),
                                              currentStop_.frameTop);
        runToStopOrExit(emit);
        return true;
    }

    if (command == "disconnect" || command == "terminate") {
        if (session_) {
            // Let the program finish if it's parked.
            if (!terminated_) session_->controller().resume();
            session_->join();  // also detaches handlers + active controller
        }
        // The program thread has joined, so nothing can read the environment
        // any more: put back what the launch displaced (spec 4.1.4). The scope
        // also restores from its destructor, which covers the abnormal exits
        // that never reach this line.
        envScope_.restore();
        // Resident lifecycle (resident-debug-server 1.2.1): this ends the
        // SESSION, not the process — reset every per-session member so the
        // next initialize/launch starts exactly like a fresh process would.
        // The PROCESS ends at stdin EOF (run()'s read loop) or on the
        // launcher killing it; a lingering idle server costs only memory.
        session_.reset();
        launchOpts_ = cajeta::jit::JitRunOptions{};
        stopOnEntry_ = false;
        launchEnv_.clear();
        inheritSystemEnv_ = true;
        breakpoints_.clear();
        conditions_.clear();
        exceptionsArmed_ = false;
        currentStop_ = {};
        frameTable_.clear();
        varRefToFrame_.clear();
        nextVarRef_ = 1;
        haveStop_ = false;
        terminated_ = false;
        exitCode_ = 0;
        emit(makeResponse(seq_++, requestSeq, command, true, Json::object()));
        return true;
    }

    // Unknown request: reply unsuccessfully but keep going.
    emit(makeResponse(seq_++, requestSeq, command, false,
                      Json(std::string("unsupported request: " + command))));
    return true;
}

int DapServer::run(std::istream& in, std::ostream& out) {
    Emit emit = [this, &out](const Json& msg) {
        std::lock_guard<std::mutex> lock(emitMutex_);
        writeMessage(out, msg);
    };
    Json request;
    while (readMessage(in, &request)) {
        if (!handle(request, emit)) break;
    }
    return exitCode_;
}

int DapServer::runOverStdio() {
#ifndef _WIN32
    int protoFd = ::dup(STDOUT_FILENO);
    int pfd[2] = {-1, -1};
    if (protoFd >= 0 && ::pipe(pfd) == 0) {
        // fd 1 now feeds the pump; the protocol owns a private descriptor.
        ::dup2(pfd[1], STDOUT_FILENO);
        ::close(pfd[1]);
        static __gnu_cxx::stdio_filebuf<char> protoBuf(protoFd, std::ios::out);
        static std::ostream protoStream(&protoBuf);

        // Pump: everything the debuggee (or stray host code) prints becomes
        // a DAP output event, category "stdout" — frame-atomic under the
        // emit lock. Detached: it blocks in read() for the process lifetime
        // (our own fd 1 keeps the pipe writable, so no EOF before exit).
        int rd = pfd[0];
        std::thread([this, rd]() {
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(rd, buf, sizeof buf);
                if (n <= 0) break;
                Json body = Json::object();
                body["category"] = "stdout";
                body["output"] = std::string(buf, (size_t) n);
                std::lock_guard<std::mutex> lock(emitMutex_);
                writeMessage(protoStream, makeEvent(seq_++, "output",
                                                    std::move(body)));
            }
        }).detach();

        return run(std::cin, protoStream);
    }
    if (protoFd >= 0) ::close(protoFd);
#endif
    return run(std::cin, std::cout);
}

} // namespace cajeta::dap
