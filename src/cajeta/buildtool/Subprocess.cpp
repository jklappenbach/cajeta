#include "cajeta/buildtool/Subprocess.h"

#include <cstring>

#if defined(_WIN32)
#  include <cstdlib>
#  include <fstream>
#  include <string>
#  include <windows.h>
#else
#  include <cerrno>
#  include <sys/wait.h>
#  include <unistd.h>
extern char** environ;
#endif

namespace cajeta {
namespace buildtool {

#if !defined(_WIN32)
// ---------------------------------------------------------------------------
// POSIX backend: fork + exec + waitpid. Consolidates the per-action pipe
// plumbing that used to be copy-pasted across the build-tool actions.
// ---------------------------------------------------------------------------
namespace {

// Write the whole buffer to fd, restarting on EINTR / short writes. Returns
// false on a hard error.
bool writeAll(int fd, const std::string& data) {
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

// Read fd to EOF, appending into out.
void drainFd(int fd, std::string& out) {
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
}

}  // namespace

SubprocessResult runSubprocess(const SubprocessOptions& opt) {
    SubprocessResult r;
    if (opt.argv.empty()) {
        r.error = "runSubprocess: empty argv";
        return r;
    }

    const bool capStdin = opt.stdinData != nullptr;
    const bool capOut = opt.outData != nullptr;
    const bool capErr = opt.errData != nullptr;

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};

    auto closeFd = [](int& fd) { if (fd >= 0) { ::close(fd); fd = -1; } };
    auto closeAll = [&]() {
        closeFd(inPipe[0]);  closeFd(inPipe[1]);
        closeFd(outPipe[0]); closeFd(outPipe[1]);
        closeFd(errPipe[0]); closeFd(errPipe[1]);
    };

    if (capStdin && ::pipe(inPipe) < 0) {
        r.error = std::string("pipe(stdin): ") + std::strerror(errno);
        return r;
    }
    if (capOut && ::pipe(outPipe) < 0) {
        closeAll();
        r.error = std::string("pipe(stdout): ") + std::strerror(errno);
        return r;
    }
    if (capErr && ::pipe(errPipe) < 0) {
        closeAll();
        r.error = std::string("pipe(stderr): ") + std::strerror(errno);
        return r;
    }

    // argv as mutable C strings.
    std::vector<std::string> argStore = opt.argv;
    std::vector<char*> argv;
    argv.reserve(argStore.size() + 1);
    for (auto& a : argStore) argv.push_back(a.data());
    argv.push_back(nullptr);

    std::vector<std::string> envStore;
    std::vector<char*> envp;
    const bool haveEnv = opt.env && !opt.env->empty();
    if (haveEnv) {
        envStore = *opt.env;
        envp.reserve(envStore.size() + 1);
        for (auto& e : envStore) envp.push_back(e.data());
        envp.push_back(nullptr);
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        closeAll();
        r.error = std::string("fork: ") + std::strerror(errno);
        return r;
    }

    if (pid == 0) {
        // Child. Wire captured streams; leave the rest inherited.
        if (capStdin) ::dup2(inPipe[0], STDIN_FILENO);
        if (capOut)   ::dup2(outPipe[1], STDOUT_FILENO);
        if (capErr)   ::dup2(errPipe[1], STDERR_FILENO);
        // Close every pipe fd in the child (the dups above kept the std fds).
        ::close(inPipe[0]);  ::close(inPipe[1]);
        ::close(outPipe[0]); ::close(outPipe[1]);
        ::close(errPipe[0]); ::close(errPipe[1]);

        if (opt.cwd && !opt.cwd->empty()) {
            if (::chdir(opt.cwd->c_str()) != 0) {
                std::string msg = "chdir('" + *opt.cwd + "'): " +
                                  std::strerror(errno) + "\n";
                (void)!::write(STDERR_FILENO, msg.data(), msg.size());
                _exit(127);
            }
        }

        // Swap in the replacement environment (if any) and exec. Setting the
        // global `environ` before execvp is portable across glibc and macOS,
        // where execvpe is unavailable.
        if (haveEnv) environ = envp.data();
        ::execvp(argv[0], argv.data());

        std::string msg = std::string("exec '") + argStore[0] + "': " +
                          std::strerror(errno) + "\n";
        (void)!::write(STDERR_FILENO, msg.data(), msg.size());
        _exit(127);
    }

    // Parent. Close child-side ends.
    closeFd(inPipe[0]);
    closeFd(outPipe[1]);
    closeFd(errPipe[1]);

    r.launched = true;

    if (capStdin) {
        writeAll(inPipe[1], *opt.stdinData);
        closeFd(inPipe[1]);
    }
    if (capOut) { drainFd(outPipe[0], *opt.outData); closeFd(outPipe[0]); }
    if (capErr) { drainFd(errPipe[0], *opt.errData); closeFd(errPipe[0]); }

    int status = 0;
    for (;;) {
        pid_t w = ::waitpid(pid, &status, 0);
        if (w < 0) {
            if (errno == EINTR) continue;
            // Already launched; report the wait failure but keep launched=true.
            r.error = std::string("waitpid: ") + std::strerror(errno);
            return r;
        }
        break;
    }

    if (WIFEXITED(status)) {
        r.exited = true;
        r.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        r.signaled = true;
        r.signal = WTERMSIG(status);
    }
    return r;
}

#else
// ---------------------------------------------------------------------------
// Windows backend: CreateProcess + anonymous pipes. No fork; the child's stdio
// is wired through STARTUPINFO handles.
// ---------------------------------------------------------------------------
namespace {

// Quote one argument per the MSVC C-runtime command-line parsing rules so the
// child reconstructs argv exactly. (Backslashes only matter immediately before
// a quote; an arg with no spaces/quotes is passed verbatim.)
void appendQuoted(std::string& cmd, const std::string& arg) {
    const bool needQuotes = arg.empty() ||
        arg.find_first_of(" \t\n\v\"") != std::string::npos;
    if (!needQuotes) { cmd += arg; return; }
    cmd += '"';
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            cmd.append(backslashes * 2 + 1, '\\');
            cmd += '"';
            backslashes = 0;
        } else {
            if (backslashes) { cmd.append(backslashes, '\\'); backslashes = 0; }
            cmd += c;
        }
    }
    if (backslashes) cmd.append(backslashes * 2, '\\');  // escape before closing "
    cmd += '"';
}

std::string buildCommandLine(const std::vector<std::string>& argv) {
    std::string cmd;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += ' ';
        appendQuoted(cmd, argv[i]);
    }
    return cmd;
}

// Resolve argv[0] to a concrete executable path. If it already names an
// existing file, use it as-is; otherwise search PATH with a default .exe
// extension (mirrors execvp's PATH lookup for bare names like "curl").
std::string resolveExecutable(const std::string& prog) {
    DWORD attrs = ::GetFileAttributesA(prog.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES &&
        !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return prog;
    }
    char found[MAX_PATH];
    DWORD n = ::SearchPathA(nullptr, prog.c_str(), ".exe",
                            MAX_PATH, found, nullptr);
    if (n > 0 && n < MAX_PATH) return std::string(found, n);
    return prog;  // let CreateProcess fail and report it
}

// The MSYS2 root the toolchain ships in; the POSIX shells live under usr\bin.
std::string msys2Root() {
    if (const char* r = std::getenv("MSYS2_ROOT")) {
        if (*r) return r;
    }
    return "C:\\msys64";
}

// Windows CreateProcess can't launch a "#!"-script directly. Peek at `path`;
// if it begins with a shebang, return the Windows interpreter to run it with
// (the script then becomes the interpreter's first argument). Returns "" when
// there is no shebang. POSIX interpreters map onto the MSYS2 shells the build
// already depends on; this lets the build tool run shell-script plugins, test
// binaries, and exec actions on Windows just as it does on POSIX.
std::string shebangInterpreter(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    char magic[2] = {0, 0};
    in.read(magic, 2);
    if (in.gcount() != 2 || magic[0] != '#' || magic[1] != '!') return "";
    std::string line;
    std::getline(in, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // line is the rest of the shebang, e.g. "/bin/sh" or "/usr/bin/env bash".
    auto has = [&](const char* needle) {
        return line.find(needle) != std::string::npos;
    };
    const std::string root = msys2Root();
    if (has("bash")) return root + "\\usr\\bin\\bash.exe";
    if (has("sh"))   return root + "\\usr\\bin\\sh.exe";
    if (has("python")) return root + "\\usr\\bin\\python.exe";
    if (has("perl")) return root + "\\usr\\bin\\perl.exe";
    return root + "\\usr\\bin\\sh.exe";  // default to sh
}

// Convert a Windows path to forward slashes — the MSYS2 shells accept this form
// for a script argument, whereas backslashes can be read as escapes.
std::string toForwardSlashes(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

// Double-null-terminated environment block from "KEY=VALUE" entries.
std::string buildEnvBlock(const std::vector<std::string>& env) {
    std::string block;
    for (const auto& e : env) { block += e; block.push_back('\0'); }
    block.push_back('\0');
    return block;
}

// A std handle to hand the child for a stream we are NOT capturing. Falls back
// to NUL when the parent has no valid handle (e.g. a detached process). Sets
// `created` when the returned handle is one we own and must close afterwards
// (the NUL fallback); a real std handle is left for the parent to keep using.
HANDLE inheritedStdHandle(DWORD which, bool& created) {
    created = false;
    HANDLE h = ::GetStdHandle(which);
    if (h && h != INVALID_HANDLE_VALUE) {
        ::SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        return h;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    DWORD access = (which == STD_INPUT_HANDLE) ? GENERIC_READ : GENERIC_WRITE;
    created = true;
    return ::CreateFileA("NUL", access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         &sa, OPEN_EXISTING, 0, nullptr);
}

void readToEnd(HANDLE h, std::string& out) {
    char buf[4096];
    DWORD n = 0;
    while (::ReadFile(h, buf, sizeof(buf), &n, nullptr) && n > 0) {
        out.append(buf, n);
    }
}

}  // namespace

SubprocessResult runSubprocess(const SubprocessOptions& opt) {
    SubprocessResult r;
    if (opt.argv.empty()) {
        r.error = "runSubprocess: empty argv";
        return r;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE inRd = nullptr, inWr = nullptr;
    HANDLE outRd = nullptr, outWr = nullptr;
    HANDLE errRd = nullptr, errWr = nullptr;
    bool nulIn = false, nulOut = false, nulErr = false;

    auto fail = [&](const std::string& msg) {
        auto closeH = [](HANDLE h) { if (h && h != INVALID_HANDLE_VALUE) ::CloseHandle(h); };
        // Close only handles we own: pipe ends we created, and the NUL
        // fallbacks. NEVER an inherited real std handle (closing the process's
        // own stdout/stderr/stdin corrupts it and crashes later output).
        if (opt.stdinData) { closeH(inRd); closeH(inWr); }
        else if (nulIn)    { closeH(inRd); }
        if (opt.outData)   { closeH(outRd); closeH(outWr); }
        else if (nulOut)   { closeH(outWr); }
        if (opt.errData)   { closeH(errRd); closeH(errWr); }
        else if (nulErr)   { closeH(errWr); }
        r.error = msg;
        return r;
    };

    // Create pipes only for the streams we drive; for the rest, inherit the
    // parent's std handle. The parent-side end of each pipe is marked
    // non-inheritable so the child cannot keep it open (which would wedge the
    // EOF the parent waits for).
    if (opt.stdinData) {
        if (!::CreatePipe(&inRd, &inWr, &sa, 0))
            return fail("CreatePipe(stdin) failed");
        ::SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    } else {
        inRd = inheritedStdHandle(STD_INPUT_HANDLE, nulIn);
    }
    if (opt.outData) {
        if (!::CreatePipe(&outRd, &outWr, &sa, 0))
            return fail("CreatePipe(stdout) failed");
        ::SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    } else {
        outWr = inheritedStdHandle(STD_OUTPUT_HANDLE, nulOut);
    }
    if (opt.errData) {
        if (!::CreatePipe(&errRd, &errWr, &sa, 0))
            return fail("CreatePipe(stderr) failed");
        ::SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);
    } else {
        errWr = inheritedStdHandle(STD_ERROR_HANDLE, nulErr);
    }

    std::string exe = resolveExecutable(opt.argv[0]);
    // Shebang emulation: if the target is a "#!"-script, run it through the
    // mapped interpreter (the script becomes the interpreter's first arg).
    std::vector<std::string> launchArgv = opt.argv;
    std::string interp = shebangInterpreter(exe);
    if (!interp.empty()) {
        launchArgv.clear();
        launchArgv.push_back(interp);
        launchArgv.push_back(toForwardSlashes(exe));  // the script
        for (size_t k = 1; k < opt.argv.size(); ++k) {
            launchArgv.push_back(opt.argv[k]);
        }
        exe = resolveExecutable(interp);
    }
    std::string cmdLine = buildCommandLine(launchArgv);
    std::vector<char> cmdMut(cmdLine.begin(), cmdLine.end());
    cmdMut.push_back('\0');

    std::string envBlock;
    LPVOID envPtr = nullptr;
    if (opt.env && !opt.env->empty()) {
        envBlock = buildEnvBlock(*opt.env);
        envPtr = envBlock.data();
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRd;
    si.hStdOutput = outWr;
    si.hStdError = errWr;

    PROCESS_INFORMATION pi{};
    BOOL ok = ::CreateProcessA(
        exe.c_str(), cmdMut.data(), nullptr, nullptr,
        /*bInheritHandles=*/TRUE, 0, envPtr,
        (opt.cwd && !opt.cwd->empty()) ? opt.cwd->c_str() : nullptr,
        &si, &pi);
    if (!ok) {
        return fail("CreateProcess('" + opt.argv[0] + "') failed: error " +
                    std::to_string(::GetLastError()));
    }
    r.launched = true;

    // Release the handles the child now owns its own copy of, so the parent
    // doesn't hold the write/read end that EOF depends on. For captured streams
    // that's the child-side pipe end; for inherited streams it's only the NUL
    // fallback we created (a real std handle stays open for the parent's use).
    auto closeIf = [](HANDLE& h) { if (h && h != INVALID_HANDLE_VALUE) { ::CloseHandle(h); h = nullptr; } };
    if (opt.stdinData)   closeIf(inRd);   // child's stdin read end
    else if (nulIn)      closeIf(inRd);   // NUL fallback we own
    if (opt.outData)     closeIf(outWr);  // child's stdout write end
    else if (nulOut)     closeIf(outWr);
    if (opt.errData)     closeIf(errWr);  // child's stderr write end
    else if (nulErr)     closeIf(errWr);

    if (opt.stdinData) {
        const char* p = opt.stdinData->data();
        size_t left = opt.stdinData->size();
        while (left > 0) {
            DWORD wrote = 0;
            if (!::WriteFile(inWr, p, (DWORD)(left > 0x7fffffff ? 0x7fffffff : left),
                             &wrote, nullptr) || wrote == 0)
                break;
            p += wrote; left -= wrote;
        }
        closeIf(inWr);
    }
    if (opt.outData) { readToEnd(outRd, *opt.outData); closeIf(outRd); }
    if (opt.errData) { readToEnd(errRd, *opt.errData); closeIf(errRd); }

    ::WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD ec = 0;
    ::GetExitCodeProcess(pi.hProcess, &ec);
    r.exited = true;
    r.exitCode = static_cast<int>(ec);

    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return r;
}

#endif

}  // namespace buildtool
}  // namespace cajeta
