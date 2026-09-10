#include "cajeta/dap/DapServer.h"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <iostream>
#include <thread>
#include <cerrno>
#include <streambuf>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#endif

#include "cajeta/dap/DapProtocol.h"
#include "cajeta/error/Diagnostics.h"
#include "cajeta/dbg/DebugLocTable.h"
#include "cajeta/dbg/ValueInspector.h"

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
    // Clients read the reason from DAP's TOP-LEVEL `message`, not from `body`.
    if (!success && body.isString() && !body.asString().empty())
        r["message"] = body.asString();
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

    Json meta = Json::object();
    meta["alloc"] = cajeta::dbg::allocClassName(v.alloc);
    meta["ownership"] = cajeta::dbg::ownershipRoleName(v.ownership);
    meta["lifetime"] = cajeta::dbg::lifetimeStateName(v.lifetime);
    var["cajeta"] = std::move(meta);

    // A moved-out binding is consumed, so it must not look editable.
    if (v.lifetime == LifetimeState::MovedOut) {
        Json attrs = Json::array();
        attrs.push_back(std::string("readOnly"));
        Json hint = Json::object();
        hint["attributes"] = std::move(attrs);
        var["presentationHint"] = std::move(hint);
    }
    return var;
}

// One child row (element or field): no facets, since those belong to a declared
// binding rather than a drilled slot. `ref` is 0 for a leaf, else an expansion handle.
static Json childVariableJson(const std::string& name, const std::string& type,
                              const std::string& value, int ref,
                              bool isStatic = false) {
    Json var = Json::object();
    var["name"] = name;
    var["value"] = value;
    var["type"] = type;
    var["variablesReference"] = ref;
    if (isStatic) {
        Json hint = Json::object();
        Json attrs = Json::array();
        attrs.push_back(Json("static"));
        hint["attributes"] = std::move(attrs);
        var["presentationHint"] = std::move(hint);
    }
    return var;
}

// One step of a simple evaluate path — a `.field` or a `[index]`.
namespace {
    struct PathStep {
        bool isField;
        std::string field;
        size_t index = 0;
    };

    bool isIdentStart(char c) {
        return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
    }
    bool isIdentChar(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }

    // Parse a bare identifier followed only by `.field` / `[digits]` segments into
    // `root` + `steps`. Anything else returns false, so the caller says
    // "unsupported" rather than guessing; this navigates, it never evaluates.
    bool parseSimplePath(const std::string& in, std::string& root,
                         std::vector<PathStep>& steps) {
        size_t a = 0, b = in.size();
        while (a < b && std::isspace(static_cast<unsigned char>(in[a]))) a++;
        while (b > a && std::isspace(static_cast<unsigned char>(in[b - 1]))) b--;
        const std::string s = in.substr(a, b - a);
        if (s.empty() || !isIdentStart(s[0])) return false;
        size_t i = 0;
        while (i < s.size() && isIdentChar(s[i])) i++;
        root = s.substr(0, i);
        while (i < s.size()) {
            if (s[i] == '.') {
                i++;
                if (i >= s.size() || !isIdentStart(s[i])) return false;
                size_t fs = i;
                while (i < s.size() && isIdentChar(s[i])) i++;
                steps.push_back(PathStep{true, s.substr(fs, i - fs), 0});
            } else if (s[i] == '[') {
                i++;
                size_t ds = i;
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
                if (i == ds || i >= s.size() || s[i] != ']') return false;
                PathStep step{false, "", static_cast<size_t>(std::stoull(s.substr(ds, i - ds)))};
                i++;
                steps.push_back(step);
            } else {
                return false;
            }
        }
        return true;
    }
} // namespace

// Chain length from the C runtime — a pointer chase cheap enough to run per safepoint.
extern "C" int __cajeta_dbg_frame_depth(void* top);
// Chain-containment probe, of the same pure pointer-chase family.
extern "C" int __cajeta_dbg_frame_contains(void* top, void* node);

namespace {
// The step origin's "line" as an opaque key, with the file folded in so a line-number
// match in another source cannot read as "same line". -1 = unknown loc.
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
// The server's own executable path. Empty when unresolvable, so checks fail OPEN.
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

// Is the step-decision trace on? OFF unless CAJETA_STEP_TRACE is set to something
// other than 0/empty: it writes to stderr, which the IDE console paints red.
bool stepTraceEnabled() {
    static const bool on = [] {
        const char* v = ::getenv("CAJETA_STEP_TRACE");
        return v != nullptr && *v != '\0' && std::string(v) != "0";
    }();
    return on;
}

// Print one stderr line per recorded step decision: loc, fiber, depth and verdict.
void dumpStepTrace(
    const std::vector<cajeta::dbg::DebugController::StepDecision>& trace) {
    const auto& table = globalDbgLocTable();
    for (const auto& d : trace) {
        const auto& loc = table.at(d.locId);
        std::cerr << "[step-trace] loc=" << d.locId << " "
                  << loc.file << ":" << loc.line
                  << " fiber=" << d.fiberId
                  << " depth=" << d.depth
                  << " origin=" << d.originDepth
                  << " kind=" << d.kind
                  << " why=" << (d.reason == 0 ? "eval"
                               : d.reason == 1 ? "FIBER-MISMATCH"
                               : d.reason == 2 ? "same-line"
                                               : "CHAIN-MISMATCH")
                  << (d.stopped ? "  << STOP" : "") << "\n";
    }
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
    if (!now.empty()) {
        if (selfIdentityAtStart_.empty()) selfIdentityAtStart_ = now;
        else if (selfIdentityAtStart_ != now)
            return refuse("compiler binary changed on disk");
    }
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
    drainToExit();
}

// End the debuggee cleanly from either exit — `disconnect`, or destruction when the
// client vanished. Joining a PARKED program hangs, so disarm every stop source first.
void DapServer::drainToExit() {
    if (!session_) return;
    auto& controller = session_->controller();
    controller.clearArmed();
    controller.disarmException();
    controller.disarmEntry();
    controller.resume();
    // A stop already in flight when the arms were cleared can still park once.
    const auto deadline = std::chrono::steady_clock::now()
                        + std::chrono::seconds(2);
    while (!session_->isFinished()
               && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        controller.resume();
    }
    session_->join();
}

void DapServer::runToStopOrExit(const Emit& emit) {
    if (!session_) return;
    using namespace std::chrono;
    while (true) {
        StopEvent ev;
        if (session_->controller().waitForStop(ev, milliseconds(50))) {
            auto frames = cajeta::dbg::walkFrames(ev.frameTop);
            // A breakpoint whose condition is false does not stop: keep running.
            if (!shouldStopAt(ev, frames)) {
                session_->controller().resume();
                continue;
            }
            currentStop_ = ev;
            haveStop_ = true;
            rebuildFrameTable(std::move(frames));
            Json body = Json::object();
            if (stepTraceEnabled()
                && ev.reason == cajeta::dbg::StopEvent::StopReason::Step) {
                auto walked = cajeta::dbg::walkFrames(ev.frameTop);
                std::cerr << "[step-trace] STOP chain len=" << walked.size();
                for (size_t i = 0; i < walked.size() && i < 3; ++i)
                    std::cerr << " [" << i << "]=" << walked[i].func;
                std::cerr << "\n";
                dumpStepTrace(session_->controller().drainStepTrace());
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
            body["threadId"] = static_cast<int>(ev.fiberId);
            // Claim stop-the-world only if the barrier saw every carrier park.
            body["allThreadsStopped"] = (ev.unquiescedCarriers == 0);
            emit(makeEvent(seq_++, "stopped", std::move(body)));
            return;
        }
        if (session_->isFinished()) {
            if (stepTraceEnabled()) {
                auto trace = session_->controller().drainStepTrace();
                if (!trace.empty()) {
                    std::cerr << "[step-trace] program EXITED with a pending "
                                 "step; last " << trace.size()
                              << " candidates:\n";
                    dumpStepTrace(trace);
                }
            }
            exitCode_ = session_->join();
            terminated_ = true;
            haveStop_ = false;
            frameTable_.clear();
            varRefToFrame_.clear();
            varRefToAggregate_.clear();
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
    const auto& table = globalDbgLocTable();
    if (stop.locId < 0 || static_cast<size_t>(stop.locId) >= table.size())
        return true;
    const auto& loc = table.at(stop.locId);
    std::string base = std::filesystem::path(loc.file).filename().string();
    auto it = conditions_.find({base, loc.line});
    if (it == conditions_.end() || it->second.empty()) return true;
    if (frames.empty()) return true;
    std::string err;
    return cajeta::dbg::evaluateCondition(it->second, frames.front().locals,
                                          &err);
}

int DapServer::mintAggregateRef(const std::string& type, void* addr,
                                size_t start) {
    int ref = nextVarRef_++;
    varRefToAggregate_[ref] = AggregateRef{type, addr, start};
    return ref;
}

void DapServer::rebuildFrameTable(
        std::vector<cajeta::dbg::DbgFrameInfo> stoppedFrames) {
    frameTable_.clear();
    varRefToFrame_.clear();
    varRefToAggregate_.clear();
    nextVarRef_ = 1;
    const int stoppedTid = static_cast<int>(currentStop_.fiberId);

    if (!stoppedFrames.empty()) {
        for (auto& fr : stoppedFrames)
            frameTable_.push_back(FrameEntry{stoppedTid, std::move(fr)});
    } else if (currentStop_.locId >= 0) {
        // No frame chain: synthesize one from the loc table so stackTrace still works.
        const auto& table = globalDbgLocTable();
        cajeta::dbg::DbgFrameInfo fr;
        if (static_cast<size_t>(currentStop_.locId) < table.size())
            fr.func = table.at(currentStop_.locId).function;
        fr.locId = currentStop_.locId;
        frameTable_.push_back(FrameEntry{stoppedTid, std::move(fr)});
    }

    // Other fibers' chains are stable while the carrier is parked; skip the stopped one.
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
        caps["supportsEvaluateForHovers"] = true;
        Json filter = Json::object();
        filter["filter"] = "all";
        filter["label"] = "All thrown exceptions";
        filter["default"] = false;
        Json filters = Json::array();
        filters.push_back(std::move(filter));
        caps["exceptionBreakpointFilters"] = std::move(filters);
        emit(makeResponse(seq_++, requestSeq, command, true, caps));
        emit(makeEvent(seq_++, "initialized", Json::object()));
        return true;
    }

    if (command == "launch") {
        std::string entry = args.at("entry-method").asString();
        if (entry.empty()) entry = args.at("entryMethod").asString();
        // runJit wants the dotted form; the docs also spell it "Class::method".
        auto pos = entry.find("::");
        if (pos != std::string::npos) entry = entry.substr(0, pos) + "."
                                            + entry.substr(pos + 2);
        launchOpts_.entryMethod = entry;
        launchOpts_.sourceRoot = args.at("sourceRoot").asString();
        if (launchOpts_.sourceRoot.empty())
            launchOpts_.sourceRoot = args.at("source-root").asString();
        launchOpts_.cacheDir = args.at("cacheDir").asString();
        if (launchOpts_.cacheDir.empty())
            launchOpts_.cacheDir = args.at("cache-dir").asString();
        launchOpts_.resident = args.at("resident").isBool()
                                   ? args.at("resident").asBool()
                                   : false;
        // DI profile for @Profile providers (absence = the compiler default).
        launchOpts_.profile = args.at("profile").asString();
        launchOpts_.profile = args.at("profile").asString();
        // The JIT's --classpath: an array of paths, or one comma-separated string.
        launchOpts_.classpath.clear();
        const Json& cp = args.at("classpath");
        if (cp.isArray()) {
            for (const auto& e : cp.elements())
                if (!e.asString().empty())
                    launchOpts_.classpath.push_back(e.asString());
        } else if (!cp.asString().empty()) {
            std::string all = cp.asString();
            size_t start = 0;
            while (start <= all.size()) {
                size_t comma = all.find(',', start);
                std::string one = all.substr(
                    start, comma == std::string::npos ? std::string::npos : comma - start);
                if (!one.empty()) launchOpts_.classpath.push_back(one);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        stopOnEntry_ = args.at("stopOnEntry").asBool();
        // Absence of "env" means UNSPECIFIED, not "empty": reading it as empty would
        // blank the debuggee's. The inherit flag likewise defaults on.
        launchEnv_.clear();
        const Json& env = args.at("env");
        if (env.isObject())
            for (const auto& kv : env.items())
                launchEnv_[kv.first] = kv.second.asString();
        inheritSystemEnv_ = args.at("inheritSystemEnv").isBool()
                                ? args.at("inheritSystemEnv").asBool()
                                : true;
        // Elements per page when expanding an array; missing or <= 0 keeps the default.
        int reqPage = args.at("pageSize").asInt();
        if (reqPage <= 0) reqPage = args.at("page-size").asInt();
        if (reqPage > 0) pageSize_ = static_cast<size_t>(reqPage);
        emit(makeResponse(seq_++, requestSeq, command, true, Json::object()));
        return true;
    }

    if (command == "setBreakpoints") {
        std::string path = args.at("source").at("path").asString();
        if (path.empty()) path = args.at("source").at("name").asString();
        std::string base = std::filesystem::path(path).filename().string();
        // Whole-file REPLACE per the DAP spec, or a re-send leaves stale copies.
        std::vector<cajeta::jit::Breakpoint> dropped;
        for (size_t i = breakpoints_.size(); i-- > 0; ) {
            if (breakpoints_[i].file != base) continue;
            dropped.push_back(breakpoints_[i]);
            breakpoints_.erase(breakpoints_.begin() + i);
            if (i < breakpointIds_.size())
                breakpointIds_.erase(breakpointIds_.begin() + i);
        }
        Json verified = Json::array();
        const Json& bps = args.at("breakpoints");
        for (size_t i = 0; i < bps.size(); ++i) {
            int line = bps[i].at("line").asInt();
            breakpoints_.push_back(cajeta::jit::Breakpoint{base, line});
            std::string cond = bps[i].at("condition").asString();
            if (!cond.empty()) conditions_[{base, line}] = cond;
            else conditions_.erase({base, line});
            // Optimistic: nothing is compiled, so configurationDone corrects this.
            const int id = nextBreakpointId_++;
            breakpointIds_.push_back(id);
            Json b = Json::object();
            b["id"] = id;
            b["verified"] = true;
            b["line"] = line;
            verified.push_back(std::move(b));
        }
        // Tell a LIVE session: `breakpoints_` is consumed exactly once, at
        // configurationDone, so the edit above would otherwise never reach it.
        // Re-arm the CURRENT WHOLE set — two sources can share a locId by basename.
        if (session_) {
            auto& controller = session_->controller();
            for (const auto& bp : dropped)
                for (int32_t id : cajeta::jit::matchingLocIds(bp))
                    controller.disarm(id);
            for (const auto& bp : breakpoints_)
                for (int32_t id : cajeta::jit::matchingLocIds(bp))
                    controller.arm(id);
        }

        Json body = Json::object();
        body["breakpoints"] = std::move(verified);
        emit(makeResponse(seq_++, requestSeq, command, true, std::move(body)));
        return true;
    }

    if (command == "setExceptionBreakpoints") {
        // Non-empty arms break-on-throw, empty disarms; applied at configurationDone.
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
        // Applied in startDebugSession's beforeRun hook — after the build, before the
        // program thread — so the compile still runs under the real environment.
        auto applyEnv = [this]() {
            if (!launchEnv_.empty() || !inheritSystemEnv_)
                envScope_.apply(launchEnv_, inheritSystemEnv_);
        };
        // Narrated as `output` events so a working launch never reads as a hang.
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
            // Keep the DAP `output` event, but carry a RECORD as its text under the
            // json-progress flag, so one console renders both channels alike.
            if (cajeta::jsonProgressEnabled()) {
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                Json rec = Json::object();
                rec["kind"] = std::string("progress");
                rec["phase"] = phase;
                rec["state"] = std::string(phase == "jit" ? "finish" : "start");
                rec["label"] = line;
                if (total > 0) {
                    rec["current"] = current;
                    rec["total"] = total;
                }
                line = rec.dump() + "\n";
            }
            Json body = Json::object();
            body["category"] = "console";
            body["output"] = std::move(line);
            emit(makeEvent(seq_++, "output", std::move(body)));
        };
        // Armed before the program thread starts, so an immediate throw can't race it.
        session_ = cajeta::jit::startDebugSession(launchOpts_, breakpoints_,
                                                  &err, exceptionsArmed_,
                                                  stopOnEntry_, applyEnv);
        launchOpts_.onProgress = nullptr;
        bool ok = session_ != nullptr;
        if (ok) {
            Json body = Json::object();
            body["category"] = "console";
            if (cajeta::jsonProgressEnabled()) {
                Json rec = Json::object();
                rec["kind"] = std::string("progress");
                rec["phase"] = std::string("compile");
                rec["state"] = std::string("finish");
                rec["label"] = std::string("cajeta: compile finished");
                body["output"] = rec.dump() + "\n";
            } else {
                body["output"] = "cajeta: compile finished\n";
            }
            emit(makeEvent(seq_++, "output", std::move(body)));
            // Downgrade breakpoints the loc table matched to no safepoint, or the IDE
            // shows them armed and the run just ends with no explanation.
            for (size_t i = 0; i < breakpoints_.size(); ++i) {
                if (!cajeta::jit::matchingLocIds(breakpoints_[i]).empty())
                    continue;
                Json bp = Json::object();
                if (i < breakpointIds_.size()) bp["id"] = breakpointIds_[i];
                bp["verified"] = false;
                bp["line"] = breakpoints_[i].line;
                Json bpSrc = Json::object();
                bpSrc["name"] = breakpoints_[i].file;
                bpSrc["path"] = breakpoints_[i].file;
                bp["source"] = std::move(bpSrc);
                bp["message"] =
                    "no statement compiled at " + breakpoints_[i].file + ":"
                    + std::to_string(breakpoints_[i].line)
                    + " — the program cannot stop here";
                Json ev = Json::object();
                ev["reason"] = "changed";
                ev["breakpoint"] = std::move(bp);
                emit(makeEvent(seq_++, "breakpoint", std::move(ev)));
            }
            // The controller's depth/line seams, read only while a step is pending.
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
        // The entry thread is always id 0 ("main"); every live fiber is keyed by its
        // stable dbg id.
        // FIXME(CP6f-2d, specs/archive/carrier-quiesce-spec.md): NOT safe under the
        // multi-carrier scheduler — only the stopping carrier parks, so the fiber
        // registry can mutate while this walks it (liveFibers() is a TOCTOU).
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
        // The slice of the per-stop frame table for the requested thread. frameId is a
        // GLOBAL index into that table, and a missing threadId means the stopped one.
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
        // One "Locals" scope per frame; the minted reference is opaque, not a frameId.
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
        // The bridge needs the live DataLayout; it only exists while stopped.
        const bool haveDl = session_ != nullptr;

        // Case A: a frame's Locals scope; an aggregate local gets an expansion handle.
        auto it = varRefToFrame_.find(ref);
        if (it != varRefToFrame_.end() &&
                static_cast<size_t>(it->second) < frameTable_.size()) {
            for (const auto& v : frameTable_[it->second].info.locals) {
                std::string rendered = cajeta::dbg::formatValue(v.type, v.addr);
                int childRef = 0;
                // A moved-out binding is consumed: never deep-decode it.
                const bool consumed =
                    v.lifetime == cajeta::dbg::LifetimeState::MovedOut;
                std::string shownType = v.type;
                if (haveDl && !consumed) {
                    cajeta::dbg::ValueInspector insp(session_->dataLayout(),
                                            &session_->resolvedTypeSymbols());
                    auto dec = insp.inspect(v.type, v.addr);
                    rendered = dec.summary;
                    // The COLUMN shows the runtime type; handles keep the declared one.
                    shownType = insp.runtimeType(v.type, v.addr);
                    if (dec.kind == cajeta::dbg::ValueKind::Aggregate)
                        childRef = mintAggregateRef(v.type, v.addr);
                }
                Json var = variableJson(v, rendered);
                var["type"] = shownType;
                var["variablesReference"] = childRef;
                vars.push_back(std::move(var));
            }
        }

        // Case B: an expansion handle, one page at a time; a remainder is a "more" node.
        auto ait = varRefToAggregate_.find(ref);
        if (haveDl && ait != varRefToAggregate_.end()) {
            const AggregateRef ag = ait->second;  // copy: the map may re-hash.
            cajeta::dbg::ValueInspector insp(session_->dataLayout(),
                                            &session_->resolvedTypeSymbols());
            auto page = insp.children(ag.typeName, ag.addr, ag.start, pageSize_);
            for (const auto& child : page.children) {
                auto dec = insp.inspect(child.type, child.addr);
                int childRef = 0;
                if (dec.kind == cajeta::dbg::ValueKind::Aggregate)
                    childRef = mintAggregateRef(child.type, child.addr);
                vars.push_back(childVariableJson(child.name, child.type,
                                                 dec.summary, childRef,
                                                 child.isStatic));
            }
            if (page.remaining > 0) {
                int moreRef = mintAggregateRef(ag.typeName, ag.addr,
                                               page.nextStart);
                vars.push_back(childVariableJson(
                    "[" + std::to_string(page.remaining) + " more…]", "", "",
                    moreRef));
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
                // A moved-out binding is consumed — read-only (§6.1.3).
                if (v.lifetime == cajeta::dbg::LifetimeState::MovedOut) {
                    err = "cannot edit a moved-out value: " + name;
                    break;
                }
                ok = cajeta::dbg::writeValue(v.type, v.addr, value, &err);
                if (ok) rendered = cajeta::dbg::formatValue(v.type, v.addr);
                break;
            }
        }

        // A child reached by expansion; writeValue refuses any non-primitive target.
        auto ait = varRefToAggregate_.find(ref);
        if (!ok && session_ && ait != varRefToAggregate_.end()) {
            const AggregateRef ag = ait->second;
            cajeta::dbg::ValueInspector insp(session_->dataLayout(),
                                            &session_->resolvedTypeSymbols());
            auto page = insp.children(ag.typeName, ag.addr, ag.start, pageSize_);
            for (const auto& child : page.children) {
                if (child.name != name) continue;
                ok = cajeta::dbg::writeValue(child.type, child.addr, value, &err);
                if (ok) rendered = cajeta::dbg::formatValue(child.type, child.addr);
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

    if (command == "evaluate") {
        // Resolve a bare identifier or a simple `.field`/`[i]` path against the
        // frame's locals and decode it. Read-only: it navigates, never evaluates.
        // Unparsable is "unsupported"; unresolved is "not available".
        const std::string expr = args.at("expression").asString();
        const int fid = args.has("frameId") ? args.at("frameId").asInt() : 0;

        if (!session_) {
            emit(makeResponse(seq_++, requestSeq, command, false,
                              Json(std::string("not available: no stopped frame"))));
            return true;
        }
        if (fid < 0 || static_cast<size_t>(fid) >= frameTable_.size()) {
            emit(makeResponse(seq_++, requestSeq, command, false,
                              Json(std::string("not available: no such frame"))));
            return true;
        }

        std::string root;
        std::vector<PathStep> steps;
        if (!parseSimplePath(expr, root, steps)) {
            emit(makeResponse(seq_++, requestSeq, command, false,
                              Json("unsupported expression: " + expr)));
            return true;
        }

        std::string curType;
        void* curAddr = nullptr;
        bool found = false;
        for (const auto& v : frameTable_[fid].info.locals) {
            if (v.name == root) { curType = v.type; curAddr = v.addr; found = true; break; }
        }
        if (!found) {
            emit(makeResponse(seq_++, requestSeq, command, false,
                              Json("not available: " + root)));
            return true;
        }

        cajeta::dbg::ValueInspector insp(session_->dataLayout(),
                                            &session_->resolvedTypeSymbols());
        for (const auto& step : steps) {
            bool stepOk = false;
            if (step.isField) {
                auto page = insp.children(curType, curAddr, 0, pageSize_);
                for (const auto& c : page.children) {
                    if (c.name == step.field) {
                        curType = c.type; curAddr = c.addr; stepOk = true; break;
                    }
                }
            } else {
                auto page = insp.children(curType, curAddr, step.index, 1);
                const std::string want = "[" + std::to_string(step.index) + "]";
                for (const auto& c : page.children) {
                    if (c.name == want) {
                        curType = c.type; curAddr = c.addr; stepOk = true; break;
                    }
                }
            }
            if (!stepOk) {
                emit(makeResponse(seq_++, requestSeq, command, false,
                                  Json("not available: " + expr)));
                return true;
            }
        }

        auto dec = insp.inspect(curType, curAddr);
        int ref = (dec.kind == cajeta::dbg::ValueKind::Aggregate)
                      ? mintAggregateRef(curType, curAddr) : 0;
        Json body = Json::object();
        body["result"] = dec.summary;
        body["type"] = curType;
        body["variablesReference"] = ref;
        emit(makeResponse(seq_++, requestSeq, command, true, std::move(body)));
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
        // Only valid against the stopped fiber: otherwise fail with a message.
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
        // Origin: frame count + (file, line). An exception stop has locId -1, so its
        // line comes from the innermost frame's recorded loc.
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
        drainToExit();
        // The program thread has joined, so put back what the launch displaced.
        envScope_.restore();
        // Ends the SESSION, not the process: reset every per-session member. EOF ends run().
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
        varRefToAggregate_.clear();
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

namespace {
// Raw fd primitives, per platform (MinGW spells them `_`-prefixed in <io.h>). The pump
// below owns the debuggee's stdout so its prints never corrupt the protocol channel.
#ifdef _WIN32
inline int rawDup(int fd)                       { return ::_dup(fd); }
inline int rawDup2(int from, int to)            { return ::_dup2(from, to); }
inline int rawClose(int fd)                     { return ::_close(fd); }
inline int rawPipe(int fds[2])                  { return ::_pipe(fds, 1 << 16, _O_BINARY); }
inline long rawWrite(int fd, const void* p, size_t n) { return ::_write(fd, p, (unsigned) n); }
inline long rawRead(int fd, void* p, size_t n)  { return ::_read(fd, p, (unsigned) n); }
constexpr int kStdoutFd = 1;
#else
inline int rawDup(int fd)                       { return ::dup(fd); }
inline int rawDup2(int from, int to)            { return ::dup2(from, to); }
inline int rawClose(int fd)                     { return ::close(fd); }
inline int rawPipe(int fds[2])                  { return ::pipe(fds); }
inline long rawWrite(int fd, const void* p, size_t n) { return ::write(fd, p, n); }
inline long rawRead(int fd, void* p, size_t n)  { return ::read(fd, p, n); }
constexpr int kStdoutFd = STDOUT_FILENO;
#endif

// Write-only streambuf over a raw fd, replacing __gnu_cxx::stdio_filebuf — a libstdc++
// extension libc++ (macOS) does not have. A raw fd also keeps DAP's `\r\n` framing
// exact on Windows, where a text-mode FILE* would rewrite `\n`.
class FdOutBuf : public std::streambuf {
public:
    explicit FdOutBuf(int fd) : fd_(fd) {}

protected:
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        std::streamsize done = 0;
        while (done < n) {
            long w = rawWrite(fd_, s + done, (size_t) (n - done));
            if (w <= 0) {
                if (w < 0 && errno == EINTR) { continue; }
                break;
            }
            done += w;
        }
        return done;
    }

    int_type overflow(int_type c) override {
        if (c == traits_type::eof()) { return traits_type::not_eof(c); }
        char ch = traits_type::to_char_type(c);
        return xsputn(&ch, 1) == 1 ? c : traits_type::eof();
    }

private:
    int fd_;
};
}  // namespace

int DapServer::runOverStdio() {
    int protoFd = rawDup(kStdoutFd);
    int pfd[2] = {-1, -1};
    if (protoFd >= 0 && rawPipe(pfd) == 0) {
        // fd 1 now feeds the pump; the protocol owns a private descriptor.
        rawDup2(pfd[1], kStdoutFd);
        rawClose(pfd[1]);
        // libc picks stdout's buffering from what fd 1 IS at first use, and it is now
        // a pipe (4KB block), which would strand the debuggee's prints. Force line
        // buffering — unbuffered on Windows, where _IOLBF means full buffering.
#ifdef _WIN32
        ::setvbuf(stdout, nullptr, _IONBF, 0);
#else
        ::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
        static FdOutBuf protoBuf(protoFd);
        static std::ostream protoStream(&protoBuf);

        // Everything printed becomes a DAP output event, frame-atomic under the lock.
        int rd = pfd[0];
        std::thread([this, rd]() {
            char buf[4096];
            for (;;) {
                long n = rawRead(rd, buf, sizeof buf);
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
    if (protoFd >= 0) rawClose(protoFd);
    return run(std::cin, std::cout);
}

} // namespace cajeta::dap
