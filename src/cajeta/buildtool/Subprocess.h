#ifndef CAJETA_BUILDTOOL_SUBPROCESS_H
#define CAJETA_BUILDTOOL_SUBPROCESS_H

// Portable child-process spawning for the build tool.
//
// The build-tool actions (build/test/exec/download/package/upload) and the
// plugin runtime all need to run an external program, optionally feeding it
// stdin and capturing stdout/stderr. POSIX does this with fork+exec+waitpid;
// Windows has no fork, so the same surface is implemented over CreateProcess.
// This header is the single seam both platforms go through so the call sites
// stay OS-agnostic.

#include <string>
#include <vector>

namespace cajeta {
namespace buildtool {

/// What to run and how its stdio is wired. Pointers are optional inputs/outputs
/// owned by the caller; a null pointer means "use the default".
struct SubprocessOptions {
    /// Program + arguments. `argv[0]` is the program; it is resolved against
    /// PATH (POSIX execvp / Windows SearchPath) when it is not already a path
    /// to an existing file. Must be non-empty.
    std::vector<std::string> argv;

    /// Working directory for the child. Null => inherit the parent's cwd.
    const std::string* cwd = nullptr;

    /// Full replacement environment as "KEY=VALUE" entries. Null or empty =>
    /// the child inherits the parent's environment.
    const std::vector<std::string>* env = nullptr;

    /// Bytes written to the child's stdin (then stdin is closed). Null => the
    /// child inherits the parent's stdin.
    const std::string* stdinData = nullptr;

    /// When non-null, the child's stdout is captured here instead of inheriting
    /// the parent's stdout.
    std::string* outData = nullptr;

    /// When non-null, the child's stderr is captured here instead of inheriting
    /// the parent's stderr.
    std::string* errData = nullptr;
};

/// Outcome of a spawn. `launched` distinguishes "could not start the process"
/// (error set) from "process ran and returned a status".
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
