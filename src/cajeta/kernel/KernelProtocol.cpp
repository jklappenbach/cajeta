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

        // True, filling `out`, only when the rendering IS json: a scalar published
        // as `application/json` makes frontends render a JSON view of a number.
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

        // The request whose output is streaming. Guarded because the capture PUMP
        // thread reads it while the execution thread swaps it on every cell.
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
            // IOPub is a PUB socket: a left-over identity becomes a bogus topic frame.
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

        // The running session, published for the IO thread: `session` belongs to the
        // execution thread, and this is the only pointer an interrupt may follow.
        std::atomic<KernelSession*> live{nullptr};

        KernelSession* ensureSession(std::string* error) {
            if (session) return session.get();
            if (factory) {
                session = factory(error);
            } else {
                SessionOptions options;
                options.projectDir = projectDir;
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
        // Cleared BEFORE teardown: an interrupt must not follow it into a corpse.
        impl_->live.store(nullptr, std::memory_order_release);
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
        // Cajeta's surface syntax is close enough that Java highlighting beats none.
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
        // wait=true holds the clear until the next output arrives.
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

        // The only place warnings reach the notebook, and only the CELL's own: the
        // session's first compile pulls stdlib diagnostics through the same bridge,
        // which an unfiltered pass would blame on the user's cell.
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
            std::string ename = result.threw
                              ? result.exceptionType
                              : (result.errorId.empty() ? "CompileError" : result.errorId);
            dap::Json traceback = dap::Json::array();
            if (result.threw) {
                for (const auto& f : result.traceback) {
                    traceback.push_back(f.text.empty() ? f.method : f.text);
                }
                // The message goes LAST in the traceback: a frontend that renders a
                // non-empty traceback drops `evalue`, so this is the only place the
                // reason is seen.
                if (!result.message.empty()) {
                    traceback.push_back(ename.empty()
                                            ? result.message
                                            : ename + ": " + result.message);
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
        // Meaningless except for `incomplete`, but frontends read it unconditionally.
        content["indent"] = indent;
        publish(channel, makeReply("is_complete_reply", request, sessionId,
                                   std::move(content)));
    }

    void KernelProtocol::handle(Channel channel, const JupyterMessage& request) {
        Impl& impl = *impl_;
        const std::string type = request.type();

        // An unknown verb is dropped WITHOUT the busy/idle pair: a busy with no idle
        // leaves the frontend waiting on work that is never happening.
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
            // Reply BEFORE teardown, or a clean exit reads to the frontend as a death.
            impl.publish(channel, makeReply("shutdown_reply", request,
                                            impl.sessionId, std::move(content)));
            impl.shutdown = true;
            impl.restart = restart;
        } else if (type == "interrupt_request") {
            // Reached only when NO cell is running; the transport answers the busy
            // case on its IO thread, since this queue never drains during a cell.
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
            // No completion engine yet; an empty well-formed reply keeps Tab alive.
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
