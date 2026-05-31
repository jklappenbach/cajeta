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
            currentStop_ = ev;
            haveStop_ = true;
            Json body = Json::object();
            body["reason"] = "breakpoint";
            body["threadId"] = 1;
            body["allThreadsStopped"] = true;
            emit(makeEvent(seq_++, "stopped", std::move(body)));
            return;
        }
        if (session_->isFinished()) {
            exitCode_ = session_->join();
            terminated_ = true;
            haveStop_ = false;
            Json body = Json::object();
            body["exitCode"] = exitCode_;
            emit(makeEvent(seq_++, "exited", body));
            emit(makeEvent(seq_++, "terminated", Json::object()));
            return;
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

    if (command == "configurationDone") {
        std::string err;
        session_ = cajeta::jit::startDebugSession(launchOpts_, breakpoints_, &err);
        bool ok = session_ != nullptr;
        emit(makeResponse(seq_++, requestSeq, command, ok,
                          ok ? Json::object() : Json(err)));
        if (ok) runToStopOrExit(emit);
        return true;
    }

    if (command == "threads") {
        Json threads = Json::array();
        Json t = Json::object();
        t["id"] = 1;
        t["name"] = "main";
        threads.push_back(std::move(t));
        Json body = Json::object();
        body["threads"] = std::move(threads);
        emit(makeResponse(seq_++, requestSeq, command, true, std::move(body)));
        return true;
    }

    if (command == "stackTrace") {
        Json body = haveStop_
            ? stackTraceBody(currentStop_, globalDbgLocTable())
            : Json::object();
        if (!haveStop_) {
            body["stackFrames"] = Json::array();
            body["totalFrames"] = 0;
        }
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
