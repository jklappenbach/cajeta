//
// jupyter-kernel U5 (spec 3.1-3.4, 4.1-4.4) — the protocol verbs.
//
// This layer knows Jupyter and knows KernelSession. It does NOT know ZeroMQ:
// every outbound message goes to a `Sink` tagged with its channel, so the
// whole verb surface is exercisable in a test with a vector for a sink, and
// the transport underneath it stays a thing that moves bytes.
//
// The ordering of what a request emits is protocol, not taste. An
// `execute_request` publishes, in this order:
//
//   IOPub  status(busy)          — the frontend's spinner starts here
//   IOPub  execute_input         — echo, so every frontend agrees on In[N]
//   IOPub  stream*               — live, while the cell is still running
//   IOPub  execute_result        — only when the cell HAS a unit result
//   IOPub  error                 — only when it failed
//   Shell  execute_reply         — ok/error, with the same execution_count
//   IOPub  status(idle)          — spinner stops; the frontend may send more
//
// `status(idle)` last is what tells a frontend the kernel is free. Emitting
// the reply after it makes cells appear to finish out of order.
//
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "cajeta/kernel/JupyterMessage.h"

namespace cajeta::kernel {

    class KernelSession;

    class KernelProtocol {
    public:
        using Sink = std::function<void(Channel, const JupyterMessage&)>;
        // Builds the JIT session. Injectable so a test can supply a stub, and
        // so restart is "throw the old one away and call this again" rather
        // than a second construction path.
        using SessionFactory =
            std::function<std::unique_ptr<KernelSession>(std::string* error)>;

        explicit KernelProtocol(Sink sink);
        ~KernelProtocol();
        KernelProtocol(const KernelProtocol&) = delete;
        KernelProtocol& operator=(const KernelProtocol&) = delete;

        void setSessionFactory(SessionFactory factory);

        // The project whose `cajeta.json` classpath cells compile against
        // (spec 6). `cajeta kernel` sets this to its working directory —
        // Jupyter launches a kernel in the notebook's own directory, so that
        // is the project the user means. Applies to the next session built,
        // so a restart picks up a manifest edited in the meantime.
        void setProjectDir(std::string dir);
        // The kernel's own session id, stamped into every header we
        // originate. Distinct from the CLIENT's session, which rides in the
        // parent header.
        void setSessionId(std::string id);

        // Dispatch one verified inbound request. Unknown message types are
        // ignored (the protocol's own rule): a kernel that faults on a verb
        // it has not implemented cannot be extended by its frontend.
        void handle(Channel channel, const JupyterMessage& request);

        // Set by `shutdown_request`. The transport polls this to leave its
        // loop; `restartRequested` distinguishes a restart (fresh session,
        // same process) from a stop.
        bool shutdownRequested() const;
        bool restartRequested() const;

        // 1-based count of executes SEEN, failures included (spec 2.2).
        int executionCount() const;

        // Stop the running cell at its next safepoint (spec 5.1), if one is
        // running. SAFE FROM ANOTHER THREAD, and it has to be: the transport
        // answers `interrupt_request` on its IO thread precisely because the
        // execution thread is busy inside the cell being interrupted. Only
        // touches the session through KernelSession::requestInterrupt, which
        // is itself documented as cross-thread safe.
        void interrupt();

        // Tear the session down and build a new one. `shutdown_request` with
        // restart=true does this itself; the transport calls it when the
        // frontend restarts out-of-band.
        void restartSession();

        // spec 3.1 — the `kernel_info_reply` content. Static because a
        // frontend gets an answer before any session exists: kernel_info must
        // not pay for a JIT.
        static dap::Json kernelInfo();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace cajeta::kernel
