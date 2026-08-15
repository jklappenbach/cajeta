#include "cajeta/kernel/ZmqTransport.h"

#ifdef CAJETA_HAVE_ZMQ
#include <zmq.h>
#endif

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "cajeta/kernel/JupyterMessage.h"
#include "cajeta/kernel/KernelProtocol.h"

namespace cajeta::kernel {

#ifdef CAJETA_HAVE_ZMQ

    namespace {

        // One inbound or outbound multipart message, already reduced to the
        // only thing a socket cares about: an ordered list of byte frames.
        struct Envelope {
            Channel channel = Channel::Shell;
            std::vector<std::string> frames;
        };

        // A queue with a shutdown state, so a waiting consumer wakes on
        // teardown rather than blocking on a producer that is already gone.
        class EnvelopeQueue {
        public:
            void push(Envelope e) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    items_.push_back(std::move(e));
                }
                cv_.notify_one();
            }

            // Blocks until an item is available or the queue closes. False
            // means closed and drained.
            bool pop(Envelope* out) {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { return !items_.empty() || closed_; });
                if (items_.empty()) return false;
                *out = std::move(items_.front());
                items_.pop_front();
                return true;
            }

            // Non-blocking drain, for the IO thread's poll turn.
            std::vector<Envelope> drain() {
                std::lock_guard<std::mutex> lock(mutex_);
                std::vector<Envelope> out(std::make_move_iterator(items_.begin()),
                                          std::make_move_iterator(items_.end()));
                items_.clear();
                return out;
            }

            void close() {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    closed_ = true;
                }
                cv_.notify_all();
            }

        private:
            std::mutex mutex_;
            std::condition_variable cv_;
            std::deque<Envelope> items_;
            bool closed_ = false;
        };

        bool recvMultipart(void* socket, std::vector<std::string>* frames) {
            frames->clear();
            for (;;) {
                zmq_msg_t part;
                if (zmq_msg_init(&part) != 0) return false;
                int n = zmq_msg_recv(&part, socket, ZMQ_DONTWAIT);
                if (n < 0) {
                    zmq_msg_close(&part);
                    return !frames->empty();
                }
                frames->emplace_back(static_cast<const char*>(zmq_msg_data(&part)),
                                     zmq_msg_size(&part));
                int more = 0;
                size_t moreSize = sizeof(more);
                zmq_getsockopt(socket, ZMQ_RCVMORE, &more, &moreSize);
                zmq_msg_close(&part);
                if (!more) return true;
            }
        }

        bool sendMultipart(void* socket, const std::vector<std::string>& frames) {
            for (size_t i = 0; i < frames.size(); ++i) {
                int flags = (i + 1 < frames.size()) ? ZMQ_SNDMORE : 0;
                if (zmq_send(socket, frames[i].data(), frames[i].size(), flags) < 0) {
                    return false;
                }
            }
            return true;
        }

        // The port a socket actually bound to, which is the only way to learn
        // it when the endpoint asked for `:*`.
        int boundPort(void* socket) {
            char endpoint[256];
            size_t len = sizeof(endpoint);
            if (zmq_getsockopt(socket, ZMQ_LAST_ENDPOINT, endpoint, &len) != 0) {
                return 0;
            }
            std::string ep(endpoint, len > 0 ? len - 1 : 0);
            size_t colon = ep.rfind(':');
            if (colon == std::string::npos) return 0;
            return std::atoi(ep.c_str() + colon + 1);
        }

    }  // namespace

#endif  // CAJETA_HAVE_ZMQ

    // The connection file itself needs no ZeroMQ: `cajeta init --kernel`
    // writes kernel.json on a build with no transport, and a frontend can
    // still be pointed at a kernel built elsewhere.

    std::string ConnectionInfo::endpoint(int port) const {
        std::ostringstream out;
        out << transport << "://" << ip << ':';
        if (port > 0) out << port;
        else out << '*';
        return out.str();
    }

    bool ConnectionInfo::load(const std::string& path, ConnectionInfo* out,
                              std::string* error) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error) *error = "cannot open connection file: " + path;
            return false;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        bool ok = false;
        dap::Json json = dap::Json::parse(buffer.str(), &ok);
        if (!ok || !json.isObject()) {
            if (error) *error = "connection file is not a JSON object: " + path;
            return false;
        }
        ConnectionInfo info;
        info.transport = json.at("transport").asString(info.transport);
        info.ip = json.at("ip").asString(info.ip);
        info.shellPort = json.at("shell_port").asInt();
        info.iopubPort = json.at("iopub_port").asInt();
        info.stdinPort = json.at("stdin_port").asInt();
        info.controlPort = json.at("control_port").asInt();
        info.hbPort = json.at("hb_port").asInt();
        info.key = json.at("key").asString();
        info.signatureScheme =
            json.at("signature_scheme").asString(info.signatureScheme);
        info.kernelName = json.at("kernel_name").asString(info.kernelName);
        if (info.shellPort == 0 || info.iopubPort == 0) {
            if (error) *error = "connection file names no shell/iopub port: " + path;
            return false;
        }
        if (out) *out = std::move(info);
        return true;
    }

    std::string ConnectionInfo::toJson() const {
        dap::Json json = dap::Json::object();
        json["transport"] = transport;
        json["ip"] = ip;
        json["shell_port"] = shellPort;
        json["iopub_port"] = iopubPort;
        json["stdin_port"] = stdinPort;
        json["control_port"] = controlPort;
        json["hb_port"] = hbPort;
        json["key"] = key;
        json["signature_scheme"] = signatureScheme;
        json["kernel_name"] = kernelName;
        return json.dump();
    }

    bool ConnectionInfo::write(const std::string& path, std::string* error) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) *error = "cannot write connection file: " + path;
            return false;
        }
        out << toJson() << '\n';
        return true;
    }

#ifdef CAJETA_HAVE_ZMQ

    struct KernelTransport::Impl {
        void* context = nullptr;
        void* shell = nullptr;
        void* control = nullptr;
        void* iopub = nullptr;
        void* stdinSock = nullptr;
        void* heartbeat = nullptr;

        ConnectionInfo info;
        MessageSigner signer;

        EnvelopeQueue inbound;    // IO thread -> execution thread
        EnvelopeQueue outbound;   // any thread -> IO thread
        std::atomic<bool> running{true};

        std::thread hbThread;
        // Published by the execution thread once its protocol exists, so the
        // IO thread can answer an interrupt without going through the queue.
        std::atomic<KernelProtocol*> protocol{nullptr};

        // U6 / spec 5.1 + 6.3.1 — `interrupt_request` is answered HERE, on
        // the IO thread, and never queued. Queueing it would put it behind
        // the very cell it is meant to stop: the execution thread cannot
        // drain its queue while it is inside a runaway loop, so the request
        // would arrive only once the loop finished, which is exactly never.
        // This is what "the kernel stays responsive during the stuck window"
        // means in practice.
        bool answerOnIoThread(const Envelope& in) {
            if (in.channel != Channel::Control) return false;
            JupyterMessage msg;
            if (!decodeMessage(in.frames, signer, &msg, nullptr)) return false;
            if (msg.type() != "interrupt_request") return false;
            if (KernelProtocol* p = protocol.load(std::memory_order_acquire)) {
                p->interrupt();
            }
            Envelope out;
            out.channel = Channel::Control;
            out.frames = encodeMessage(
                makeReply("interrupt_reply", msg, sessionId,
                          dap::Json::object()), signer);
            outbound.push(std::move(out));
            return true;
        }

        std::string sessionId = newUuid();

        void* openSocket(int type, int port, int* boundOut, std::string* error) {
            void* sock = zmq_socket(context, type);
            if (!sock) {
                if (error) *error = "zmq_socket failed";
                return nullptr;
            }
            // Drop queued messages on close rather than blocking the
            // destructor on a frontend that has already gone away.
            int linger = 0;
            zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
            std::string ep = info.endpoint(port);
            if (zmq_bind(sock, ep.c_str()) != 0) {
                if (error) *error = "cannot bind " + ep + ": " + zmq_strerror(errno);
                zmq_close(sock);
                return nullptr;
            }
            if (boundOut) *boundOut = boundPort(sock);
            return sock;
        }

        void* socketFor(Channel channel) {
            switch (channel) {
                case Channel::Shell:   return shell;
                case Channel::Control: return control;
                case Channel::IOPub:   return iopub;
                case Channel::Stdin:   return stdinSock;
            }
            return nullptr;
        }

        // The heartbeat is a bare REP echo. ipykernel uses zmq_proxy for
        // this; a poll loop costs the same and can actually be stopped.
        void runHeartbeat() {
            while (running.load()) {
                zmq_pollitem_t item{heartbeat, 0, ZMQ_POLLIN, 0};
                int n = zmq_poll(&item, 1, 100);
                if (n <= 0) continue;
                std::vector<std::string> frames;
                if (recvMultipart(heartbeat, &frames)) {
                    sendMultipart(heartbeat, frames);
                }
            }
        }

        void runExecution() {
            KernelProtocol protocol([this](Channel channel, const JupyterMessage& msg) {
                Envelope out;
                out.channel = channel;
                out.frames = encodeMessage(msg, signer);
                outbound.push(std::move(out));
            });
            protocol.setSessionId(sessionId);
            // Spec 6 wants the notebook's own directory to be the project,
            // and `setProjectDir(cwd)` is the one line that does it — but it
            // is NOT set yet, deliberately. A session with a classpath
            // currently fails codegen outright (plan 7.2.5), so defaulting
            // this to the cwd would break `cajeta kernel` for every user who
            // launched Jupyter inside a project with dependencies, which is
            // most of them. Turn it on when 7.2.5 lands, not before.
            this->protocol.store(&protocol, std::memory_order_release);

            Envelope envelope;
            while (inbound.pop(&envelope)) {
                JupyterMessage msg;
                std::string error;
                // Spec 3.2: an unverifiable message is dropped where it is
                // decoded. No reply, no log line the sender controls, and
                // certainly no dispatch.
                if (!decodeMessage(envelope.frames, signer, &msg, &error)) continue;
                protocol.handle(envelope.channel, msg);
                if (protocol.shutdownRequested()) {
                    if (protocol.restartRequested()) {
                        // Restart in-process: a fresh session, an empty
                        // binding table, and the counter back to 1 (spec
                        // 3.3). The frontend keeps its connection.
                        protocol.restartSession();
                        continue;
                    }
                    break;
                }
            }
            running.store(false);
            outbound.close();
        }
    };

    KernelTransport::KernelTransport() : impl_(std::make_unique<Impl>()) {}

    KernelTransport::~KernelTransport() {
        Impl& impl = *impl_;
        impl.running.store(false);
        impl.inbound.close();
        impl.outbound.close();
        if (impl.hbThread.joinable()) impl.hbThread.join();
        for (void* sock : {impl.shell, impl.control, impl.iopub,
                           impl.stdinSock, impl.heartbeat}) {
            if (sock) zmq_close(sock);
        }
        if (impl.context) zmq_ctx_destroy(impl.context);
    }

    bool KernelTransport::bind(ConnectionInfo* info, std::string* error) {
        Impl& impl = *impl_;
        impl.info = *info;
        impl.signer = MessageSigner(impl.info.key, impl.info.signatureScheme);

        impl.context = zmq_ctx_new();
        if (!impl.context) {
            if (error) *error = "zmq_ctx_new failed";
            return false;
        }

        impl.shell = impl.openSocket(ZMQ_ROUTER, impl.info.shellPort,
                                     &info->shellPort, error);
        if (!impl.shell) return false;
        impl.control = impl.openSocket(ZMQ_ROUTER, impl.info.controlPort,
                                       &info->controlPort, error);
        if (!impl.control) return false;
        impl.stdinSock = impl.openSocket(ZMQ_ROUTER, impl.info.stdinPort,
                                         &info->stdinPort, error);
        if (!impl.stdinSock) return false;
        impl.iopub = impl.openSocket(ZMQ_PUB, impl.info.iopubPort,
                                     &info->iopubPort, error);
        if (!impl.iopub) return false;
        impl.heartbeat = impl.openSocket(ZMQ_REP, impl.info.hbPort,
                                         &info->hbPort, error);
        if (!impl.heartbeat) return false;

        impl.info = *info;
        return true;
    }

    void KernelTransport::stop() {
        impl_->running.store(false);
        impl_->inbound.close();
    }

    int KernelTransport::run() {
        Impl& impl = *impl_;
        impl.hbThread = std::thread([&impl] { impl.runHeartbeat(); });
        std::thread exec([&impl] { impl.runExecution(); });

        while (impl.running.load()) {
            zmq_pollitem_t items[3] = {
                {impl.shell, 0, ZMQ_POLLIN, 0},
                {impl.control, 0, ZMQ_POLLIN, 0},
                {impl.stdinSock, 0, ZMQ_POLLIN, 0},
            };
            // A short timeout rather than a blocking poll: the same turn has
            // to flush whatever the execution and pump threads produced, and
            // cell output must not wait on the next inbound message to be
            // delivered.
            int n = zmq_poll(items, 3, 20);
            if (n > 0) {
                struct { void* sock; Channel channel; } sources[3] = {
                    {impl.shell, Channel::Shell},
                    {impl.control, Channel::Control},
                    {impl.stdinSock, Channel::Stdin},
                };
                for (int i = 0; i < 3; ++i) {
                    if (!(items[i].revents & ZMQ_POLLIN)) continue;
                    Envelope in;
                    in.channel = sources[i].channel;
                    if (recvMultipart(sources[i].sock, &in.frames)) {
                        // An interrupt is answered here rather than queued —
                        // see answerOnIoThread.
                        if (impl.answerOnIoThread(in)) continue;
                        impl.inbound.push(std::move(in));
                    }
                }
            }
            for (auto& out : impl.outbound.drain()) {
                void* sock = impl.socketFor(out.channel);
                if (sock) sendMultipart(sock, out.frames);
            }
        }

        impl.inbound.close();
        if (exec.joinable()) exec.join();
        // Whatever the execution thread produced on its way out — the
        // shutdown_reply above all — still has to reach the frontend.
        for (auto& out : impl.outbound.drain()) {
            void* sock = impl.socketFor(out.channel);
            if (sock) sendMultipart(sock, out.frames);
        }
        impl.running.store(false);
        if (impl.hbThread.joinable()) impl.hbThread.join();
        return 0;
    }

#else  // !CAJETA_HAVE_ZMQ

    // No transport in this build. The verb refuses with a message naming the
    // package to install rather than crashing on a null socket, and every
    // other part of the kernel — session, protocol, connection file — is
    // still built and still tested.
    struct KernelTransport::Impl {};

    KernelTransport::KernelTransport() : impl_(std::make_unique<Impl>()) {}
    KernelTransport::~KernelTransport() = default;

    bool KernelTransport::bind(ConnectionInfo*, std::string* error) {
        if (error) {
            *error = "this cajeta was built without libzmq, so `cajeta kernel` "
                     "is unavailable; install libzmq3-dev (or zeromq) and "
                     "rebuild";
        }
        return false;
    }

    int KernelTransport::run() { return 1; }
    void KernelTransport::stop() {}

#endif  // CAJETA_HAVE_ZMQ

}  // namespace cajeta::kernel
