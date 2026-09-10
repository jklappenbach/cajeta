// The Jupyter protocol verbs. Knows KernelSession, not ZeroMQ: every outbound message
// goes to a channel-tagged `Sink`. An execute_request emits IOPub status(busy),
// execute_input, stream/result/error, the Shell reply, then IOPub status(idle) LAST.
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
        // Builds the JIT session; injectable, so restart is just calling it again.
        using SessionFactory =
            std::function<std::unique_ptr<KernelSession>(std::string* error)>;

        explicit KernelProtocol(Sink sink);
        ~KernelProtocol();
        KernelProtocol(const KernelProtocol&) = delete;
        KernelProtocol& operator=(const KernelProtocol&) = delete;

        void setSessionFactory(SessionFactory factory);

        // The project whose `cajeta.json` classpath cells compile against; `cajeta kernel`
        // uses its cwd. Applies to the NEXT session, so a restart picks up manifest edits.
        void setProjectDir(std::string dir);
        // The kernel's own session id, distinct from the CLIENT's in the parent header.
        void setSessionId(std::string id);

        // Dispatch one verified inbound request; an unknown message type is ignored.
        void handle(Channel channel, const JupyterMessage& request);

        // Polled by the transport to leave its loop; restart keeps the process alive.
        bool shutdownRequested() const;
        bool restartRequested() const;

        // 1-based count of executes SEEN, failures included.
        int executionCount() const;

        // Stop the running cell at its next safepoint. SAFE FROM ANOTHER THREAD, and it
        // has to be: the transport answers `interrupt_request` on its IO thread precisely
        // because the execution thread is inside the cell being interrupted.
        void interrupt();

        // Tear the session down and build a new one, for an out-of-band frontend restart.
        void restartSession();

        // The `kernel_info_reply` content. Static, so it never pays for a JIT session.
        static dap::Json kernelInfo();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}  // namespace cajeta::kernel
