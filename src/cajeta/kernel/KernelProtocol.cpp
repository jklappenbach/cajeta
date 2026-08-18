#include "cajeta/kernel/KernelProtocol.h"

#include "cajeta/kernel/CellCompleteness.h"
#include "cajeta/kernel/KernelSession.h"

#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#ifndef CAJETA_VERSION
#define CAJETA_VERSION "0.0.0-unknown"
#endif

namespace cajeta::kernel {

    namespace {

        // A rendered value accompanies its text with `application/json` only
        // when the rendering IS json — i.e. the value's own shape round-trips
        // (spec 4.3). Publishing `application/json: "42"` for a scalar would
        // have structured-output frontends render a JSON view of a number,
        // which is noise dressed as capability.
        bool structuredForm(const std::string& text, dap::Json* out) {
            if (text.empty()) return false;
            char first = '\0';
            for (char c : text) {
                if (!std::isspace(static_cast<unsigned char>(c))) { first = c; break; }
            }
            if (first != '{' && first != '[') return false;
            bool ok = false;
            dap::Json parsed = dap::Json::parse(text, &ok);
            if (!ok || !(parsed.isObject() || parsed.isArray())) return false;
            if (out) *out = std::move(parsed);
            return true;
        }

    }  // namespace

    struct KernelProtocol::Impl {
        Sink sink;
        SessionFactory factory;
        std::string sessionId = "cajeta-kernel";
        std::string projectDir;
        std::unique_ptr<KernelSession> session;
        int executionCount = 0;
        bool shutdown = false;
        bool restart = false;

        // The request whose output is currently streaming. Read on the
        // capture PUMP thread (FdCapture calls the stream handler there), so
        // it is guarded — the execution thread swaps it on every cell.
        std::mutex parentMutex;
        JupyterMessage parent;

        void publish(Channel channel, const JupyterMessage& msg) {
            if (sink) sink(channel, msg);
        }

        void status(const JupyterMessage& request, const char* state) {
            dap::Json content = dap::Json::object();
            content["execution_state"] = std::string(state);
            JupyterMessage msg = makeReply("status", request, sessionId,
                                           std::move(content));
            // IOPub is a PUB socket: no caller to route back to, and leaving
            // the request's identity on it would prepend a bogus topic frame.
            msg.identities.clear();
            publish(Channel::IOPub, msg);
        }

        void publishIoPub(const std::string& type, const JupyterMessage& request,
                          dap::Json content) {
            JupyterMessage msg = makeReply(type, request, sessionId,
                                           std::move(content));
            msg.identities.clear();
            publish(Channel::IOPub, msg);
        }

        void stream(const std::string& name, const std::string& text) {
            std::lock_guard<std::mutex> lock(parentMutex);
            if (text.empty()) return;
            dap::Json content = dap::Json::object();
            content["name"] = name;
            content["text"] = text;
            publishIoPub("stream", parent, std::move(content));
        }

        // The running session, published for the IO thread. `session` itself
        // is owned by the execution thread and must not be read from another
        // one — this is the single pointer an interrupt is allowed to follow,
        // and it is only non-null while a session is live.
        std::atomic<KernelSession*> live{nullptr};

        KernelSession* ensureSession(std::string* error) {
            if (session) return session.get();
            if (factory) {
                session = factory(error);
            } else {
                SessionOptions options;
                options.projectDir = projectDir;
                // 7.2.8 — the build runs inside the first execute_request;
                // narrate it so the cell reads as working, not hung.
                options.progress = [this](const std::string& phase) {
                    stream("stdout", "[session] " + phase + "…\n");
                };
                session = KernelSession::create(options, error);
            }
            live.store(session.get(), std::memory_order_release);
            if (session) {
                session->setStreamHandler([this](const std::string& chunk) {
                    stream("stdout", chunk);
                });
            }
            return session.get();
        }

        void interruptSelf() {
            if (KernelSession* s = live.load(std::memory_order_acquire)) {
                s->requestInterrupt();
            }
        }

        void handleExecute(Channel channel, const JupyterMessage& request);
        void handleIsComplete(Channel channel, const JupyterMessage& request);
    };

    KernelProtocol::KernelProtocol(Sink sink)
        : impl_(std::make_unique<Impl>()) {
        impl_->sink = std::move(sink);
    }

    KernelProtocol::~KernelProtocol() = default;

    void KernelProtocol::setSessionFactory(SessionFactory factory) {
        impl_->factory = std::move(factory);
    }

    void KernelProtocol::setSessionId(std::string id) {
        impl_->sessionId = std::move(id);
    }

    void KernelProtocol::setProjectDir(std::string dir) {
        impl_->projectDir = std::move(dir);
    }

    bool KernelProtocol::shutdownRequested() const { return impl_->shutdown; }
    bool KernelProtocol::restartRequested() const { return impl_->restart; }
    int KernelProtocol::executionCount() const { return impl_->executionCount; }

    void KernelProtocol::interrupt() {
        if (KernelSession* s = impl_->live.load(std::memory_order_acquire)) {
            s->requestInterrupt();
        }
    }

    void KernelProtocol::restartSession() {
        // Cleared BEFORE the session is torn down: an interrupt racing a
        // restart must not follow a pointer into a session being destroyed.
        impl_->live.store(nullptr, std::memory_order_release);
        // Explicit shutdown before release: the session drops its bindings
        // and joins its carriers in that order, and doing it here rather than
        // in a destructor keeps the ordering visible (spec 3.3).
        if (impl_->session) impl_->session->shutdown();
        impl_->session.reset();
        impl_->executionCount = 0;
        impl_->shutdown = false;
        impl_->restart = false;
    }

    dap::Json KernelProtocol::kernelInfo() {
        dap::Json lang = dap::Json::object();
        lang["name"] = "cajeta";
        lang["version"] = CAJETA_VERSION;
        lang["mimetype"] = "text/x-cajeta";
        lang["file_extension"] = ".cajeta";
        // Cajeta's surface syntax is close enough to Java that a frontend's
        // Java highlighter is right far more often than no highlighter.
        lang["pygments_lexer"] = "java";
        lang["codemirror_mode"] = "text/x-java";
        lang["nbconvert_exporter"] = "script";

        dap::Json info = dap::Json::object();
        info["status"] = "ok";
        info["protocol_version"] = "5.3";
        info["implementation"] = "cajeta";
        info["implementation_version"] = CAJETA_VERSION;
        info["language_info"] = lang;
        info["banner"] = std::string("cajeta ") + CAJETA_VERSION
                       + " — JIT kernel. Each cell compiles into the running "
                         "session; bindings persist across cells.";
        info["help_links"] = dap::Json::array();
        return info;
    }

    void KernelProtocol::Impl::handleExecute(Channel channel,
                                             const JupyterMessage& request) {
        const std::string code = request.content.at("code").asString();
        const bool silent = request.content.at("silent").asBool(false);

        {
            std::lock_guard<std::mutex> lock(parentMutex);
            parent = request;
        }

        // A silent request runs but leaves no trace: no counter movement, no
        // input echo, no Out[N] (protocol 5.3, `silent`).
        if (!silent) {
            ++executionCount;
            dap::Json echo = dap::Json::object();
            echo["code"] = code;
            echo["execution_count"] = executionCount;
            publishIoPub("execute_input", request, std::move(echo));
        }
        const int count = executionCount;

        std::string error;
        const bool building = !session;
        KernelSession* s = ensureSession(&error);
        // 7.2.8 — the narration served its purpose; clear it so the cell's
        // final state is only its real output. wait=true defers the clear
        // until the next output arrives, so the last phase line stays
        // visible right up to the result.
        if (building && s) {
            dap::Json clear = dap::Json::object();
            clear["wait"] = true;
            publishIoPub("clear_output", request, std::move(clear));
        }
        if (!s) {
            dap::Json err = dap::Json::object();
            err["ename"] = "KernelError";
            err["evalue"] = error.empty() ? std::string("session unavailable") : error;
            err["traceback"] = dap::Json::array();
            publishIoPub("error", request, err);

            dap::Json reply = dap::Json::object();
            reply["status"] = "error";
            reply["execution_count"] = count;
            reply["ename"] = err.at("ename").asString();
            reply["evalue"] = err.at("evalue").asString();
            reply["traceback"] = dap::Json::array();
            publish(channel, makeReply("execute_reply", request, sessionId,
                                       std::move(reply)));
            return;
        }

        CellResult result = s->execute(code, "In[" + std::to_string(count) + "]");

        // Warnings reach the notebook only here: `ok` carries no room for
        // them, and a warning the user never sees may as well not exist.
        //
        // Only the CELL's own diagnostics, though. The session's first
        // compile pulls the stdlib through the same diagnostics bridge, and
        // an unfiltered pass republishes forty-odd ownership warnings about
        // stdlib internals as if the user's two-line cell had provoked them.
        // A diagnostic that cannot name the cell it came from is not the
        // user's to read: it is compiler chatter that predates their code.
        const std::string cellName = "In[" + std::to_string(count) + "]";
        for (const auto& d : result.diagnostics) {
            if (d.severity == "error") continue;
            if (d.file != cellName) continue;
            std::string line = d.severity + ": " + d.message;
            if (!d.file.empty()) {
                line += " (" + d.file;
                if (d.line > 0) line += ", line " + std::to_string(d.line);
                line += ")";
            }
            dap::Json content = dap::Json::object();
            content["name"] = "stderr";
            content["text"] = line + "\n";
            publishIoPub("stream", request, std::move(content));
        }

        if (result.ok && result.hasResult && !silent) {
            dap::Json data = dap::Json::object();
            data["text/plain"] = result.result;
            dap::Json structured;
            if (structuredForm(result.result, &structured)) {
                data["application/json"] = std::move(structured);
            }
            dap::Json content = dap::Json::object();
            content["execution_count"] = count;
            content["data"] = std::move(data);
            content["metadata"] = dap::Json::object();
            publishIoPub("execute_result", request, std::move(content));
        }

        dap::Json reply = dap::Json::object();
        reply["execution_count"] = count;
        if (result.ok) {
            reply["status"] = "ok";
            reply["user_expressions"] = dap::Json::object();
            reply["payload"] = dap::Json::array();
        } else {
            // A compile failure and a throw look the same to the frontend —
            // both are `error` with a type, a message, and a traceback. What
            // differs is what fills them: the compiler's error id and its
            // located message, or the thrown type and its `In[N]` frames.
            std::string ename = result.threw
                              ? result.exceptionType
                              : (result.errorId.empty() ? "CompileError" : result.errorId);
            dap::Json traceback = dap::Json::array();
            if (result.threw) {
                for (const auto& f : result.traceback) {
                    traceback.push_back(f.text.empty() ? f.method : f.text);
                }
            } else {
                std::string where = result.file;
                if (result.line > 0) where += ", line " + std::to_string(result.line);
                traceback.push_back(where + ": " + result.message);
            }
            dap::Json err = dap::Json::object();
            err["ename"] = ename;
            err["evalue"] = result.message;
            err["traceback"] = traceback;
            publishIoPub("error", request, err);

            reply["status"] = "error";
            reply["ename"] = ename;
            reply["evalue"] = result.message;
            reply["traceback"] = std::move(traceback);
        }
        publish(channel, makeReply("execute_reply", request, sessionId,
                                   std::move(reply)));
    }

    void KernelProtocol::Impl::handleIsComplete(Channel channel,
                                                const JupyterMessage& request) {
        std::string indent;
        Completeness verdict =
            classifyCell(request.content.at("code").asString(), &indent);
        dap::Json content = dap::Json::object();
        content["status"] = std::string(completenessName(verdict));
        // The field is required for `incomplete` and meaningless otherwise,
        // but frontends read it unconditionally, so it is always present.
        content["indent"] = indent;
        publish(channel, makeReply("is_complete_reply", request, sessionId,
                                   std::move(content)));
    }

    void KernelProtocol::handle(Channel channel, const JupyterMessage& request) {
        Impl& impl = *impl_;
        const std::string type = request.type();

        // An unknown verb is ignored WITHOUT the busy/idle pair: the pair is
        // a promise that something is being worked on, and a frontend that
        // sees busy for a message we silently drop is owed an idle it will
        // eventually get from an unrelated request.
        const bool known =
            type == "execute_request" || type == "kernel_info_request" ||
            type == "is_complete_request" || type == "shutdown_request" ||
            type == "interrupt_request" || type == "comm_info_request" ||
            type == "history_request" || type == "complete_request" ||
            type == "inspect_request";
        if (!known) return;

        impl.status(request, "busy");

        if (type == "execute_request") {
            impl.handleExecute(channel, request);
        } else if (type == "kernel_info_request") {
            impl.publish(channel, makeReply("kernel_info_reply", request,
                                            impl.sessionId, kernelInfo()));
        } else if (type == "is_complete_request") {
            impl.handleIsComplete(channel, request);
        } else if (type == "shutdown_request") {
            const bool restart = request.content.at("restart").asBool(false);
            dap::Json content = dap::Json::object();
            content["restart"] = restart;
            // Reply BEFORE tearing anything down: a frontend that never gets
            // the reply reports the kernel as having died rather than having
            // stopped, and shows the user a crash dialog for a clean exit.
            impl.publish(channel, makeReply("shutdown_reply", request,
                                            impl.sessionId, std::move(content)));
            impl.shutdown = true;
            impl.restart = restart;
        } else if (type == "interrupt_request") {
            // Reached only when NO cell is running — a busy execution thread
            // cannot drain its queue. The transport answers this on its IO
            // thread instead (that is the case that matters, and the case
            // spec 5.1 is about); this arm covers the idle one, where per
            // spec 5.2 the request is a no-op that still gets acknowledged.
            impl.interruptSelf();
            impl.publish(channel, makeReply("interrupt_reply", request,
                                            impl.sessionId, dap::Json::object()));
        } else if (type == "comm_info_request") {
            dap::Json content = dap::Json::object();
            content["comms"] = dap::Json::object();
            content["status"] = "ok";
            impl.publish(channel, makeReply("comm_info_reply", request,
                                            impl.sessionId, std::move(content)));
        } else if (type == "history_request") {
            dap::Json content = dap::Json::object();
            content["history"] = dap::Json::array();
            content["status"] = "ok";
            impl.publish(channel, makeReply("history_reply", request,
                                            impl.sessionId, std::move(content)));
        } else if (type == "complete_request") {
            // No completion engine yet (the IDE's symbol index is a separate
            // thread of work). An empty, well-formed reply is what keeps Tab
            // from hanging the frontend.
            const std::string code = request.content.at("code").asString();
            int cursor = request.content.at("cursor_pos").asInt(
                static_cast<int>(code.size()));
            dap::Json content = dap::Json::object();
            content["matches"] = dap::Json::array();
            content["cursor_start"] = cursor;
            content["cursor_end"] = cursor;
            content["metadata"] = dap::Json::object();
            content["status"] = "ok";
            impl.publish(channel, makeReply("complete_reply", request,
                                            impl.sessionId, std::move(content)));
        } else if (type == "inspect_request") {
            dap::Json content = dap::Json::object();
            content["found"] = false;
            content["data"] = dap::Json::object();
            content["metadata"] = dap::Json::object();
            content["status"] = "ok";
            impl.publish(channel, makeReply("inspect_reply", request,
                                            impl.sessionId, std::move(content)));
        }

        impl.status(request, "idle");
    }

}  // namespace cajeta::kernel
