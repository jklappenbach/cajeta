#ifndef CAJETA_BUILDTOOL_SUBPROCESS_H
#define CAJETA_BUILDTOOL_SUBPROCESS_H

// Portable child-process spawning for the build tool: one seam over POSIX
// fork+exec+waitpid and Windows CreateProcess, so call sites stay OS-agnostic.

#include <string>
#include <vector>

namespace cajeta {
namespace buildtool {

/// What to run and how its stdio is wired. Every pointer is owned by the caller
/// and null means "inherit the parent's", never "discard".
struct SubprocessOptions {
    /// argv[0] is the program, resolved against PATH unless it is already a path.
    std::vector<std::string> argv;

    const std::string* cwd = nullptr;              ///< null => the parent's cwd
    const std::vector<std::string>* env = nullptr; ///< full "KEY=VALUE" replacement
    const std::string* stdinData = nullptr;        ///< written, then stdin closed
    std::string* outData = nullptr;                ///< non-null captures stdout
    std::string* errData = nullptr;                ///< non-null captures stderr
};

/// `launched` separates "could not start" (error set) from "ran and returned".
struct SubprocessResult {
    bool launched = false;   ///< true once the child process was created.
    bool exited = false;     ///< true if it terminated normally (WIFEXITED).
    int exitCode = -1;       ///< exit status when `exited`; else -1.
    bool signaled = false;   ///< POSIX: killed by a signal. Always false on Windows.
    int signal = 0;          ///< terminating signal when `signaled`.
    std::string error;       ///< populated when `!launched`.

    /// POSIX-style combined status used by the build-tool actions: the exit
    /// code on normal exit, 128+signal when signal-killed, -1 otherwise.
    int code() const {
        if (exited) return exitCode;
        if (signaled) return 128 + signal;
        return -1;
    }
};

/// Run a child process to completion. Never throws; spawn failures are reported
/// via `SubprocessResult::launched == false` with a message in `error`.
SubprocessResult runSubprocess(const SubprocessOptions& opt);

}  // namespace buildtool
}  // namespace cajeta

#endif  // CAJETA_BUILDTOOL_SUBPROCESS_H
