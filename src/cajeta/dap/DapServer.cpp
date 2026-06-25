#include "cajeta/dap/DapServer.h"

#include <chrono>
#include <filesystem>

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
            // CP6f-3: reason reflects breakpoint vs exception stop.
            body["reason"] =
                ev.reason == cajeta::dbg::StopEvent::StopReason::Exception
                    ? "exception" : "breakpoint";
            // CP6f-2b: the real stopped fiber id (0 = entry/main thread, >=1 a
            // spawned fiber) instead of a hard-coded 1.
            body["threadId"] = static_cast<int>(ev.fiberId);
            body["allThreadsStopped"] = true;
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
        // CP6f-3: arm break-on-throw inside startDebugSession (before the
        // program thread starts) so an immediate throw can't race past it.
        session_ = cajeta::jit::startDebugSession(launchOpts_, breakpoints_,
                                                  &err, exceptionsArmed_);
        bool ok = session_ != nullptr;
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
        // FIXME(CP6f-2d, docs/specs/carrier-quiesce-spec.md): this enumeration
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

    if (command == "disconnect" || command == "terminate") {
        if (session_) {
            // Let the program finish if it's parked.
            if (!terminated_) session_->controller().resume();
            session_->join();
        }
        emit(makeResponse(seq_++, requestSeq, command, true, Json::object()));
        return false;  // end the loop
    }

    // Unknown request: reply unsuccessfully but keep going.
    emit(makeResponse(seq_++, requestSeq, command, false,
                      Json(std::string("unsupported request: " + command))));
    return true;
}

int DapServer::run(std::istream& in, std::ostream& out) {
    Emit emit = [&out](const Json& msg) { writeMessage(out, msg); };
    Json request;
    while (readMessage(in, &request)) {
        if (!handle(request, emit)) break;
    }
    return exitCode_;
}

} // namespace cajeta::dap
