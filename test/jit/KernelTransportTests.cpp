//
// jupyter-kernel U5 (spec 3.1-3.3; plan 5.1) — the ZeroMQ transport.
//
// KernelProtocolTests covers the verbs with a vector for a sink. What is left
// is everything that only exists once real sockets are involved: that the
// five channels bind, that a DEALER's routing identity survives a round trip,
// that IOPub actually publishes, that the heartbeat answers on its own
// thread, and that `shutdown_request` ends the loop.
//
// It speaks the protocol as a client, so it is a minimal Jupyter frontend —
// which is the only way to find out whether ours is a kernel.
//

#include "gtest/gtest.h"

#ifdef CAJETA_HAVE_ZMQ

#include <zmq.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <unistd.h>
#include <string>
#include <thread>
#include <vector>

#include "cajeta/kernel/JupyterMessage.h"
#include "cajeta/kernel/ZmqTransport.h"

using cajeta::dap::Json;
using cajeta::kernel::ConnectionInfo;
using cajeta::kernel::JupyterMessage;
using cajeta::kernel::KernelTransport;
using cajeta::kernel::MessageSigner;
using cajeta::kernel::decodeMessage;
using cajeta::kernel::encodeMessage;
using cajeta::kernel::makeHeader;

namespace {

    bool sendFrames(void* socket, const std::vector<std::string>& frames) {
        for (size_t i = 0; i < frames.size(); ++i) {
            int flags = (i + 1 < frames.size()) ? ZMQ_SNDMORE : 0;
            if (zmq_send(socket, frames[i].data(), frames[i].size(), flags) < 0) {
                return false;
            }
        }
        return true;
    }

    // Waits up to `timeoutMs` for a complete multipart message.
    bool recvFrames(void* socket, std::vector<std::string>* frames,
                    int timeoutMs = 5000) {
        zmq_pollitem_t item{socket, 0, ZMQ_POLLIN, 0};
        if (zmq_poll(&item, 1, timeoutMs) <= 0) return false;
        frames->clear();
        for (;;) {
            zmq_msg_t part;
            zmq_msg_init(&part);
            if (zmq_msg_recv(&part, socket, 0) < 0) {
                zmq_msg_close(&part);
                return false;
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

    // A test-side Jupyter client: the four sockets a frontend opens, wired to
    // a running transport.
    struct Client {
        void* context = nullptr;
        void* shell = nullptr;
        void* control = nullptr;
        void* iopub = nullptr;
        void* heartbeat = nullptr;

        explicit Client(const ConnectionInfo& info) {
            context = zmq_ctx_new();
            shell = open(ZMQ_DEALER, info.endpoint(info.shellPort));
            control = open(ZMQ_DEALER, info.endpoint(info.controlPort));
            heartbeat = open(ZMQ_REQ, info.endpoint(info.hbPort));
            iopub = open(ZMQ_SUB, info.endpoint(info.iopubPort));
            zmq_setsockopt(iopub, ZMQ_SUBSCRIBE, "", 0);
            // PUB/SUB's slow joiner: a subscription is asynchronous, and
            // anything published before it lands is dropped on the floor.
            // Every real frontend has this race; it waits, so we wait.
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        ~Client() {
            for (void* s : {shell, control, iopub, heartbeat}) {
                if (s) zmq_close(s);
            }
            if (context) zmq_ctx_destroy(context);
        }

        void* open(int type, const std::string& endpoint) {
            void* sock = zmq_socket(context, type);
            int linger = 0;
            zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
            EXPECT_EQ(0, zmq_connect(sock, endpoint.c_str()))
                << "connect " << endpoint << ": " << zmq_strerror(errno);
            return sock;
        }
    };

    JupyterMessage request(const std::string& type, Json content = Json::object()) {
        JupyterMessage msg;
        msg.header = makeHeader(type, "test-client", "tester");
        msg.content = std::move(content);
        return msg;
    }

}  // namespace

// The whole loop, once, over real sockets: bind, handshake, publish, echo,
// shut down. Deliberately uses `kernel_info_request` rather than an execute —
// the transport is what is under test, and paying for a JIT session would
// make this a fifty-second test of something else.
TEST(KernelTransportTests, loopbackHandshakeAndShutdown) {
    ConnectionInfo info;
    info.key = "loopback-test-key";
    // Every port 0: the OS picks, and bind() writes back what it got. A test
    // that hard-codes ports fails whenever anything else on the machine
    // happens to hold one.
    KernelTransport transport;
    std::string error;
    ASSERT_TRUE(transport.bind(&info, &error)) << error;
    EXPECT_GT(info.shellPort, 0);
    EXPECT_GT(info.iopubPort, 0);
    EXPECT_GT(info.controlPort, 0);
    EXPECT_GT(info.hbPort, 0);
    EXPECT_GT(info.stdinPort, 0);

    std::thread loop([&transport] { transport.run(); });
    MessageSigner signer(info.key, info.signatureScheme);

    {
        Client client(info);

        // The heartbeat answers on its own thread and its own socket — the
        // property that keeps a long cell from reading as a dead kernel.
        const std::string beat = "ping";
        ASSERT_GE(zmq_send(client.heartbeat, beat.data(), beat.size(), 0), 0);
        std::vector<std::string> echoed;
        ASSERT_TRUE(recvFrames(client.heartbeat, &echoed)) << "heartbeat silent";
        ASSERT_EQ(1u, echoed.size());
        EXPECT_EQ(beat, echoed[0]);

        JupyterMessage info_req = request("kernel_info_request");
        ASSERT_TRUE(sendFrames(client.shell, encodeMessage(info_req, signer)));

        std::vector<std::string> frames;
        ASSERT_TRUE(recvFrames(client.shell, &frames)) << "no shell reply";
        JupyterMessage reply;
        ASSERT_TRUE(decodeMessage(frames, signer, &reply, &error)) << error;
        EXPECT_EQ("kernel_info_reply", reply.type());
        EXPECT_EQ("cajeta", reply.content.at("implementation").asString());
        EXPECT_EQ(info_req.msgId(), reply.parentHeader.at("msg_id").asString());

        // IOPub carried the busy/idle pair for that same request. Reading it
        // over a real SUB socket is the only way to know PUB is wired at all.
        bool sawStatus = false;
        for (int i = 0; i < 4 && !sawStatus; ++i) {
            std::vector<std::string> pub;
            if (!recvFrames(client.iopub, &pub, 2000)) break;
            JupyterMessage published;
            if (!decodeMessage(pub, signer, &published, &error)) continue;
            if (published.type() == "status") {
                EXPECT_EQ(info_req.msgId(),
                          published.parentHeader.at("msg_id").asString());
                sawStatus = true;
            }
        }
        EXPECT_TRUE(sawStatus) << "IOPub published no status for the request";

        // A message signed with the wrong key is dropped, and — the part
        // worth testing over a socket — the kernel stays up: the next
        // properly signed request still gets an answer (spec 3.2).
        MessageSigner forged("wrong-key");
        JupyterMessage bogus = request("kernel_info_request");
        ASSERT_TRUE(sendFrames(client.shell, encodeMessage(bogus, forged)));

        JupyterMessage second = request("kernel_info_request");
        ASSERT_TRUE(sendFrames(client.shell, encodeMessage(second, signer)));
        ASSERT_TRUE(recvFrames(client.shell, &frames))
            << "kernel stopped answering after a bad signature";
        ASSERT_TRUE(decodeMessage(frames, signer, &reply, &error)) << error;
        EXPECT_EQ(second.msgId(), reply.parentHeader.at("msg_id").asString())
            << "the forged request was answered";

        Json shutdown = Json::object();
        shutdown["restart"] = false;
        JupyterMessage stop = request("shutdown_request", shutdown);
        ASSERT_TRUE(sendFrames(client.control, encodeMessage(stop, signer)));
        ASSERT_TRUE(recvFrames(client.control, &frames)) << "no shutdown_reply";
        ASSERT_TRUE(decodeMessage(frames, signer, &reply, &error)) << error;
        EXPECT_EQ("shutdown_reply", reply.type());
        EXPECT_FALSE(reply.content.at("restart").asBool());
    }

    // `shutdown_request` ends the loop by itself. A test that had to call
    // stop() here would be hiding the fact that it does not.
    loop.join();
}

// The connection file is the frontend's half of the contract: it writes one,
// we read it, and a round trip must not lose a field.
TEST(KernelTransportTests, connectionFileRoundTrips) {
    ConnectionInfo info;
    info.transport = "tcp";
    info.ip = "127.0.0.1";
    info.shellPort = 51001;
    info.iopubPort = 51002;
    info.stdinPort = 51003;
    info.controlPort = 51004;
    info.hbPort = 51005;
    info.key = "abc-123";
    info.kernelName = "cajeta";

    std::string path = (std::filesystem::temp_directory_path()
                        / "cajeta-kernel-conn-test.json").string();
    std::string error;
    ASSERT_TRUE(info.write(path, &error)) << error;

    ConnectionInfo loaded;
    ASSERT_TRUE(ConnectionInfo::load(path, &loaded, &error)) << error;
    EXPECT_EQ(info.shellPort, loaded.shellPort);
    EXPECT_EQ(info.iopubPort, loaded.iopubPort);
    EXPECT_EQ(info.stdinPort, loaded.stdinPort);
    EXPECT_EQ(info.controlPort, loaded.controlPort);
    EXPECT_EQ(info.hbPort, loaded.hbPort);
    EXPECT_EQ(info.key, loaded.key);
    EXPECT_EQ("hmac-sha256", loaded.signatureScheme);
    EXPECT_EQ("tcp://127.0.0.1:51001", loaded.endpoint(loaded.shellPort));

    // A file that is not a connection file is refused rather than half-read.
    ASSERT_FALSE(ConnectionInfo::load(path + ".missing", &loaded, &error));
    EXPECT_FALSE(error.empty());
    std::filesystem::remove(path);
}

// 7.2.6 / spec §6 — A KERNEL LAUNCHED INSIDE A PROJECT ADOPTS THAT PROJECT.
//
// Jupyter starts a kernel in the notebook's own directory, so that directory's
// governing `cajeta.json` is the classpath the user means. Nothing in the
// protocol carries it: the kernel has to take it from where it was launched,
// or a notebook next to a project full of dependencies silently gets a
// stdlib-only session. The plumbing for this landed with 7.2.4 and was then
// left unwired for weeks — the whole defect is that nobody called the setter,
// so the test has to observe the LAUNCH PATH and not the plumbing.
//
// THE MANIFEST HERE IS DELIBERATELY BROKEN, and that is the probe rather than
// the subject. A well-formed manifest with no dependencies resolves to an
// empty classpath, which is indistinguishable from never having looked — so
// there would be nothing to assert. A manifest that cannot be parsed makes
// session creation fail with an error that NAMES THE DIRECTORY it read, which
// is precisely the fact under test: which project did this kernel adopt. It
// also keeps the test fast, since resolution runs before the stdlib is primed.
TEST(KernelTransportTests, aKernelAdoptsTheProjectItWasLaunchedIn) {
    namespace fs = std::filesystem;
    fs::path projectDir = fs::temp_directory_path()
                        / ("cajeta_kernel_cwd_" + std::to_string(::getpid()));
    fs::create_directories(projectDir);
    { std::ofstream(projectDir / "cajeta.json") << "{ this is not json"; }

    // chdir is process-global; restore it whatever the test does. gtest runs
    // tests sequentially within a process, so the window is this test alone.
    struct CwdGuard {
        fs::path saved;
        explicit CwdGuard(const fs::path& to) : saved(fs::current_path()) {
            fs::current_path(to);
        }
        ~CwdGuard() {
            std::error_code ec;
            fs::current_path(saved, ec);
        }
    } cwd(projectDir);

    ConnectionInfo info;
    info.key = "cwd-project-test-key";
    KernelTransport transport;
    std::string error;
    ASSERT_TRUE(transport.bind(&info, &error)) << error;

    std::thread loop([&transport] { transport.run(); });
    MessageSigner signer(info.key, info.signatureScheme);
    {
        Client client(info);

        Json content = Json::object();
        content["code"] = "1;";
        JupyterMessage exec = request("execute_request", content);
        ASSERT_TRUE(sendFrames(client.shell, encodeMessage(exec, signer)));

        std::vector<std::string> frames;
        JupyterMessage reply;
        // Generous: a kernel that did NOT adopt the project builds a
        // stdlib-only session instead, and priming that costs ~45s before it
        // can answer. The timeout must outlast the failure it is diagnosing.
        ASSERT_TRUE(recvFrames(client.shell, &frames, 120000)) << "no reply";
        ASSERT_TRUE(decodeMessage(frames, signer, &reply, &error)) << error;
        EXPECT_EQ("execute_reply", reply.type());

        // The kernel read THIS directory's manifest — the assertion is the
        // path in the message, not the failure itself.
        EXPECT_EQ("error", reply.content.at("status").asString())
            << "the kernel ignored the project it was launched in and built a "
               "stdlib-only session";
        const std::string evalue = reply.content.at("evalue").asString();
        EXPECT_NE(std::string::npos, evalue.find("bad manifest"))
            << "unexpected failure, not the manifest: " << evalue;
        EXPECT_NE(std::string::npos, evalue.find(projectDir.string()))
            << "adopted some other project: " << evalue;

        Json shutdown = Json::object();
        shutdown["restart"] = false;
        ASSERT_TRUE(sendFrames(client.control,
                               encodeMessage(request("shutdown_request", shutdown),
                                             signer)));
        recvFrames(client.control, &frames);
    }
    loop.join();

    std::error_code ec;
    fs::remove_all(projectDir, ec);
}

#endif  // CAJETA_HAVE_ZMQ
