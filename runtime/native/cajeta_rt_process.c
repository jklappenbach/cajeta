// === Cajeta runtime fragment — TEXTUALLY #included into cajeta_runtime.c ===
// cajeta.process — subprocess control over posix_spawnp, which reports an exec
// failure as launched=false. The bridge builds and marshals the cajeta objects.
#if !defined(_WIN32)
#include <spawn.h>
#include <poll.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
extern char** environ;
#if defined(__linux__)
// addchdir_np needs _GNU_SOURCE, which this TU does not set (glibc >= 2.29).
extern int posix_spawn_file_actions_addchdir_np(
    posix_spawn_file_actions_t*, const char*);
#endif

// Capture flag bits (must match cajeta.process.Stdio packing).
#define CJ_PROC_CAP_STDOUT 1
#define CJ_PROC_CAP_STDERR 2

typedef struct { char* data; size_t len; size_t cap; } cj_proc_buf;

static int cj_proc_buf_append(cj_proc_buf* b, const char* src, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < b->len + n) nc *= 2;
        char* nd = (char*) realloc(b->data, nc);
        if (!nd) return -1;
        b->data = nd;
        b->cap = nc;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

// Copy a `String*` to a fresh C string via the mode-aware __cajeta_string_cstr: a WINDOWED view's bytes are NOT at `bytes + 8`.
extern const char* __cajeta_string_cstr(void* s);
static char* cj_proc_str_dup(void* str, int64_t off_bytes, int64_t off_len) {
    (void) off_bytes;
    (void) off_len;
    if (!str) return NULL;
    const char* c = __cajeta_string_cstr(str);
    return strdup(c ? c : "");
}

// Marshal a `String[]` into a NULL-terminated char**; the stride is the whole String struct.
static char** cj_proc_strarr_dup(void* arr, int64_t str_size,
                                 int64_t off_bytes, int64_t off_len,
                                 int64_t* out_n) {
    *out_n = 0;
    if (!arr) return NULL;
    int64_t n = *(int64_t*) arr;          // count word @ 0
    if (n < 0) n = 0;
    char** v = (char**) malloc(sizeof(char*) * (size_t) (n + 1));
    if (!v) return NULL;
    char* base = (char*) arr + 8;
    for (int64_t i = 0; i < n; i++) {
        void* str = *(void**) (base + (size_t) i * (size_t) str_size);
        char* dup = cj_proc_str_dup(str, off_bytes, off_len);
        v[i] = dup ? dup : strdup("");
    }
    v[n] = NULL;
    *out_n = n;
    return v;
}

static void cj_proc_strarr_free(char** v) {
    if (!v) return;
    for (char** p = v; *p; p++) free(*p);
    free(v);
}

static void* cj_proc_bytes_to_array(const char* data, size_t len) {
    void* hdr = __cajeta_new_array_header(8, 1, (uint64_t) len);
    if (!hdr) return NULL;            // len==0 → NULL header is fine for cajeta
    if (len > 0 && data) memcpy((char*) hdr + 8, data, len);
    return hdr;
}

static void* cj_proc_build_result(
        void* vtable, int64_t res_size,
        int32_t launched, int32_t exited, int32_t exit_code,
        int32_t signaled, int32_t sig, int32_t timed_out,
        void* out_arr, void* err_arr,
        int64_t off_launched, int64_t off_exited, int64_t off_exitcode,
        int64_t off_signaled, int64_t off_signal, int64_t off_timedout,
        int64_t off_stdout, int64_t off_stderr) {
    void* r = __cajeta_alloc((uint64_t) res_size);
    *(void**)   ((char*) r)                = vtable;       // vtable @ 0
    *(int32_t*) ((char*) r + off_launched) = launched;
    *(int32_t*) ((char*) r + off_exited)   = exited;
    *(int32_t*) ((char*) r + off_exitcode) = exit_code;
    *(int32_t*) ((char*) r + off_signaled) = signaled;
    *(int32_t*) ((char*) r + off_signal)   = sig;
    *(int32_t*) ((char*) r + off_timedout) = timed_out;
    *(void**)   ((char*) r + off_stdout)   = out_arr;      // may be NULL
    *(void**)   ((char*) r + off_stderr)   = err_arr;
    return r;
}

static long cj_proc_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long) ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

void* __cajeta_proc_run(
        void* argv_arr,          // cajeta String[] (non-null, non-empty)
        void* cwd_str,           // cajeta String* or NULL (inherit cwd)
        void* env_arr,           // cajeta String[] "KEY=VALUE" or NULL (inherit)
        void* stdin_arr,         // cajeta int8[] or NULL (inherit stdin)
        int32_t stdin_len,
        int32_t capture_flags,   // CJ_PROC_CAP_*
        int64_t timeout_ms,      // <= 0 : no timeout
        int64_t str_size, int64_t str_off_bytes, int64_t str_off_len,
        void* result_vtable, int64_t res_size,
        int64_t off_launched, int64_t off_exited, int64_t off_exitcode,
        int64_t off_signaled, int64_t off_signal, int64_t off_timedout,
        int64_t off_stdout, int64_t off_stderr) {
#define CJ_FAILRES() cj_proc_build_result(result_vtable, res_size, \
        0, 0, -1, 0, 0, 0, NULL, NULL, \
        off_launched, off_exited, off_exitcode, off_signaled, off_signal, \
        off_timedout, off_stdout, off_stderr)

    int cap_out = (capture_flags & CJ_PROC_CAP_STDOUT) != 0;
    int cap_err = (capture_flags & CJ_PROC_CAP_STDERR) != 0;
    const char* cwd = NULL;
    char* cwd_dup = NULL;
    if (cwd_str) {
        cwd_dup = cj_proc_str_dup(cwd_str, str_off_bytes, str_off_len);
        cwd = cwd_dup;
    }
    int want_stdin = (stdin_arr != NULL && stdin_len > 0);

    int64_t argc = 0;
    char** cargv = cj_proc_strarr_dup(argv_arr, str_size,
                                      str_off_bytes, str_off_len, &argc);
    if (!cargv || argc == 0) {
        cj_proc_strarr_free(cargv);
        free(cwd_dup);
        return CJ_FAILRES();
    }
    int64_t envc = 0;
    char** cenv_built = cj_proc_strarr_dup(env_arr, str_size,
                                           str_off_bytes, str_off_len, &envc);
    char** cenv = cenv_built ? cenv_built : environ;

    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if ((want_stdin && pipe(in_pipe) != 0) ||
        (cap_out && pipe(out_pipe) != 0) ||
        (cap_err && pipe(err_pipe) != 0)) {
        goto fail;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (want_stdin) {
        posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
        posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
        posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    }
    if (cap_out) {
        posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
        posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
        posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
    }
    if (cap_err) {
        posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
        posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
        posix_spawn_file_actions_addclose(&fa, err_pipe[1]);
    }
    if (cwd) {
#if defined(__linux__)
        if (posix_spawn_file_actions_addchdir_np(&fa, cwd) != 0) {
            posix_spawn_file_actions_destroy(&fa);
            goto fail;
        }
#else
        posix_spawn_file_actions_destroy(&fa);   // cwd unsupported on this OS
        goto fail;
#endif
    }

    pid_t pid;
    int rc = posix_spawnp(&pid, cargv[0], &fa, NULL, cargv, cenv);
    posix_spawn_file_actions_destroy(&fa);

    if (want_stdin) { close(in_pipe[0]); in_pipe[0] = -1; }
    if (cap_out)    { close(out_pipe[1]); out_pipe[1] = -1; }
    if (cap_err)    { close(err_pipe[1]); err_pipe[1] = -1; }

    if (rc != 0) {
        if (in_pipe[1]  >= 0) close(in_pipe[1]);
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
        goto fail;                       // launch failure → launched=false
    }

    if (want_stdin) {
        const char* p = (const char*) stdin_arr + 8;   // past count word
        int64_t rem = stdin_len;
        while (rem > 0) {
            ssize_t n = write(in_pipe[1], p, (size_t) rem);
            if (n < 0) { if (errno == EINTR) continue; break; }
            p += n; rem -= n;
        }
        close(in_pipe[1]); in_pipe[1] = -1;
    }

    // Both streams drain concurrently, or a full pipe buffer wedges the child.
    cj_proc_buf ob = {0, 0, 0}, eb = {0, 0, 0};
    int timed_out = 0;
    long deadline = (timeout_ms > 0) ? cj_proc_now_ms() + (long) timeout_ms : 0;
    char tmp[8192];
    while ((cap_out && out_pipe[0] >= 0) || (cap_err && err_pipe[0] >= 0)) {
        struct pollfd pfds[2];
        int oi = -1, ei = -1;
        nfds_t nf = 0;
        if (cap_out && out_pipe[0] >= 0) {
            pfds[nf].fd = out_pipe[0]; pfds[nf].events = POLLIN; oi = (int) nf++;
        }
        if (cap_err && err_pipe[0] >= 0) {
            pfds[nf].fd = err_pipe[0]; pfds[nf].events = POLLIN; ei = (int) nf++;
        }
        int timeout_arg = -1;
        if (timeout_ms > 0) {
            long remaining = deadline - cj_proc_now_ms();
            if (remaining <= 0) { timed_out = 1; break; }
            timeout_arg = (int) remaining;
        }
        int pr = poll(pfds, nf, timeout_arg);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr == 0) { timed_out = 1; break; }
        if (oi >= 0 && (pfds[oi].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(out_pipe[0], tmp, sizeof tmp);
            if (n > 0) cj_proc_buf_append(&ob, tmp, (size_t) n);
            else { close(out_pipe[0]); out_pipe[0] = -1; }
        }
        if (ei >= 0 && (pfds[ei].revents & (POLLIN | POLLHUP | POLLERR))) {
            ssize_t n = read(err_pipe[0], tmp, sizeof tmp);
            if (n > 0) cj_proc_buf_append(&eb, tmp, (size_t) n);
            else { close(err_pipe[0]); err_pipe[0] = -1; }
        }
    }
    if (out_pipe[0] >= 0) { close(out_pipe[0]); out_pipe[0] = -1; }
    if (err_pipe[0] >= 0) { close(err_pipe[0]); err_pipe[0] = -1; }

    if (timed_out) kill(pid, SIGKILL);

    if (!timed_out && timeout_ms > 0 && !cap_out && !cap_err) {
        for (;;) {
            int st;
            pid_t w = waitpid(pid, &st, WNOHANG);
            if (w == pid) break;
            if (w < 0) { if (errno == EINTR) continue; break; }
            if (cj_proc_now_ms() >= deadline) {
                kill(pid, SIGKILL); timed_out = 1; break;
            }
            struct timespec nap = {0, 2 * 1000 * 1000};   // 2 ms
            nanosleep(&nap, NULL);
        }
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { /* retry */ }

    int exited = 0, exit_code = -1, signaled = 0, sig = 0;
    if (WIFEXITED(status))   { exited = 1;   exit_code = WEXITSTATUS(status); }
    if (WIFSIGNALED(status)) { signaled = 1; sig = WTERMSIG(status); }

    void* out_arr = cap_out ? cj_proc_bytes_to_array(ob.data, ob.len) : NULL;
    void* err_arr = cap_err ? cj_proc_bytes_to_array(eb.data, eb.len) : NULL;
    free(ob.data);
    free(eb.data);

    cj_proc_strarr_free(cargv);
    cj_proc_strarr_free(cenv_built);
    free(cwd_dup);
    return cj_proc_build_result(result_vtable, res_size,
        1, exited, exit_code, signaled, sig, timed_out, out_arr, err_arr,
        off_launched, off_exited, off_exitcode, off_signaled, off_signal,
        off_timedout, off_stdout, off_stderr);

fail:
    if (in_pipe[0]  >= 0) close(in_pipe[0]);
    if (in_pipe[1]  >= 0) close(in_pipe[1]);
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);
    cj_proc_strarr_free(cargv);
    cj_proc_strarr_free(cenv_built);
    free(cwd_dup);
    return CJ_FAILRES();
#undef CJ_FAILRES
}

// ---- Streaming spawn: a long-lived child on live pipes. No drop destructor, so close() reaps.

#define CJ_PROC_PIPE_STDIN  8
#define CJ_PROC_PIPE_STDOUT 16
#define CJ_PROC_PIPE_STDERR 32

typedef struct {
    pid_t pid;
    int stdin_fd;    // parent's write end, or -1
    int stdout_fd;   // parent's read end, or -1
    int stderr_fd;   // parent's read end, or -1
    int reaped;      // 1 once waitpid has collected it
    int status;      // cached wait status once reaped
} cj_proc_handle;

// Spawn and BUILD the cajeta Process object; on launch failure, handle 0 and fds -1.
void* __cajeta_proc_spawn(
        void* argv_arr, void* cwd_str, void* env_arr,
        int32_t stdio_flags,
        int64_t str_size, int64_t str_off_bytes, int64_t str_off_len,
        void* proc_vtable, int64_t proc_size,
        int64_t off_handle, int64_t off_stdin_fd,
        int64_t off_stdout_fd, int64_t off_stderr_fd) {
    int pipe_in  = (stdio_flags & CJ_PROC_PIPE_STDIN)  != 0;
    int pipe_out = (stdio_flags & CJ_PROC_PIPE_STDOUT) != 0;
    int pipe_err = (stdio_flags & CJ_PROC_PIPE_STDERR) != 0;

    int64_t argc = 0;
    char** cargv = cj_proc_strarr_dup(argv_arr, str_size,
                                      str_off_bytes, str_off_len, &argc);
    char* cwd_dup = cwd_str
        ? cj_proc_str_dup(cwd_str, str_off_bytes, str_off_len) : NULL;
    int64_t envc = 0;
    char** cenv_built = cj_proc_strarr_dup(env_arr, str_size,
                                           str_off_bytes, str_off_len, &envc);
    char** cenv = cenv_built ? cenv_built : environ;

    int in_pipe[2]  = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    int ok = (cargv && argc > 0);
    if (ok && pipe_in  && pipe(in_pipe)  != 0) ok = 0;
    if (ok && pipe_out && pipe(out_pipe) != 0) ok = 0;
    if (ok && pipe_err && pipe(err_pipe) != 0) ok = 0;

    pid_t pid = -1;
    int spawned = 0;
    if (ok) {
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        if (pipe_in) {
            posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
            posix_spawn_file_actions_addclose(&fa, in_pipe[0]);
            posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
        }
        if (pipe_out) {
            posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
            posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
            posix_spawn_file_actions_addclose(&fa, out_pipe[1]);
        }
        if (pipe_err) {
            posix_spawn_file_actions_adddup2(&fa, err_pipe[1], 2);
            posix_spawn_file_actions_addclose(&fa, err_pipe[0]);
            posix_spawn_file_actions_addclose(&fa, err_pipe[1]);
        }
        int cwd_ok = 1;
        if (cwd_dup) {
#if defined(__linux__)
            cwd_ok = (posix_spawn_file_actions_addchdir_np(&fa, cwd_dup) == 0);
#else
            cwd_ok = 0;
#endif
        }
        if (cwd_ok) {
            spawned = (posix_spawnp(&pid, cargv[0], &fa, NULL, cargv, cenv) == 0);
        }
        posix_spawn_file_actions_destroy(&fa);
    }

    if (in_pipe[0]  >= 0) close(in_pipe[0]);
    if (out_pipe[1] >= 0) close(out_pipe[1]);
    if (err_pipe[1] >= 0) close(err_pipe[1]);

    cj_proc_handle* h = NULL;
    if (spawned) {
        h = (cj_proc_handle*) malloc(sizeof(cj_proc_handle));
        if (h) {
            h->pid = pid;
            h->stdin_fd  = pipe_in  ? in_pipe[1]  : -1;
            h->stdout_fd = pipe_out ? out_pipe[0] : -1;
            h->stderr_fd = pipe_err ? err_pipe[0] : -1;
            h->reaped = 0;
            h->status = 0;
        }
    }
    if (!h) {
        if (in_pipe[1]  >= 0) close(in_pipe[1]);
        if (out_pipe[0] >= 0) close(out_pipe[0]);
        if (err_pipe[0] >= 0) close(err_pipe[0]);
    }

    cj_proc_strarr_free(cargv);
    cj_proc_strarr_free(cenv_built);
    free(cwd_dup);

    void* p = __cajeta_alloc((uint64_t) proc_size);
    *(void**)   ((char*) p)                 = proc_vtable;
    *(int64_t*) ((char*) p + off_handle)    = (int64_t) (intptr_t) h;  // 0 on failure
    *(int32_t*) ((char*) p + off_stdin_fd)  = h ? h->stdin_fd  : -1;
    *(int32_t*) ((char*) p + off_stdout_fd) = h ? h->stdout_fd : -1;
    *(int32_t*) ((char*) p + off_stderr_fd) = h ? h->stderr_fd : -1;
    return p;
}

int32_t __cajeta_proc_pid(int64_t handle_i) {
    cj_proc_handle* h = (cj_proc_handle*) (intptr_t) handle_i;
    return h ? (int32_t) h->pid : -1;
}

void __cajeta_proc_kill(int64_t handle_i) {
    cj_proc_handle* h = (cj_proc_handle*) (intptr_t) handle_i;
    if (h && !h->reaped) kill(h->pid, SIGKILL);
}

// Wait: timeout_ms <= 0 blocks and reaps; > 0 returns timedOut WITHOUT killing.
void* __cajeta_proc_wait(
        int64_t handle_i, int64_t timeout_ms,
        void* result_vtable, int64_t res_size,
        int64_t off_launched, int64_t off_exited, int64_t off_exitcode,
        int64_t off_signaled, int64_t off_signal, int64_t off_timedout,
        int64_t off_stdout, int64_t off_stderr) {
    cj_proc_handle* h = (cj_proc_handle*) (intptr_t) handle_i;
    if (!h) {
        return cj_proc_build_result(result_vtable, res_size,
            0, 0, -1, 0, 0, 0, NULL, NULL,
            off_launched, off_exited, off_exitcode, off_signaled,
            off_signal, off_timedout, off_stdout, off_stderr);
    }
    int st = h->status;
    int timed_out = 0;
    if (!h->reaped) {
        if (timeout_ms > 0) {
            long deadline = cj_proc_now_ms() + (long) timeout_ms;
            for (;;) {
                pid_t w = waitpid(h->pid, &st, WNOHANG);
                if (w == h->pid) { h->reaped = 1; h->status = st; break; }
                if (w < 0) { if (errno == EINTR) continue; break; }
                if (cj_proc_now_ms() >= deadline) { timed_out = 1; break; }
                struct timespec nap = {0, 2 * 1000 * 1000};   // 2 ms
                nanosleep(&nap, NULL);
            }
        } else {
            while (waitpid(h->pid, &st, 0) < 0 && errno == EINTR) { /* retry */ }
            h->reaped = 1; h->status = st;
        }
    }
    int exited = 0, exit_code = -1, signaled = 0, sig = 0;
    if (h->reaped) {
        if (WIFEXITED(h->status))   { exited = 1;   exit_code = WEXITSTATUS(h->status); }
        if (WIFSIGNALED(h->status)) { signaled = 1; sig = WTERMSIG(h->status); }
    }
    return cj_proc_build_result(result_vtable, res_size,
        1, exited, exit_code, signaled, sig, timed_out, NULL, NULL,
        off_launched, off_exited, off_exitcode, off_signaled, off_signal,
        off_timedout, off_stdout, off_stderr);
}

// Reap + free, killing a still-running child first so close never leaks a zombie.
void __cajeta_proc_release(int64_t handle_i) {
    cj_proc_handle* h = (cj_proc_handle*) (intptr_t) handle_i;
    if (!h) return;
    if (!h->reaped) {
        kill(h->pid, SIGKILL);
        int st;
        while (waitpid(h->pid, &st, 0) < 0 && errno == EINTR) { /* retry */ }
        h->reaped = 1;
    }
    if (h->stdin_fd  >= 0) close(h->stdin_fd);
    if (h->stdout_fd >= 0) close(h->stdout_fd);
    if (h->stderr_fd >= 0) close(h->stderr_fd);
    free(h);
}
#else
// Windows: v1 stubs report launched=false until the CreateProcess bridge lands.
void* __cajeta_proc_run(
        void* argv_arr, void* cwd_str, void* env_arr, void* stdin_arr,
        int32_t stdin_len, int32_t capture_flags, int64_t timeout_ms,
        int64_t str_size, int64_t str_off_bytes, int64_t str_off_len,
        void* result_vtable, int64_t res_size,
        int64_t off_launched, int64_t off_exited, int64_t off_exitcode,
        int64_t off_signaled, int64_t off_signal, int64_t off_timedout,
        int64_t off_stdout, int64_t off_stderr) {
    (void) argv_arr; (void) cwd_str; (void) env_arr; (void) stdin_arr;
    (void) stdin_len; (void) capture_flags; (void) timeout_ms;
    (void) str_size; (void) str_off_bytes; (void) str_off_len;
    void* r = __cajeta_alloc((uint64_t) res_size);
    *(void**)   ((char*) r)                = result_vtable;
    *(int32_t*) ((char*) r + off_launched) = 0;
    *(int32_t*) ((char*) r + off_exited)   = 0;
    *(int32_t*) ((char*) r + off_exitcode) = -1;
    *(int32_t*) ((char*) r + off_signaled) = 0;
    *(int32_t*) ((char*) r + off_signal)   = 0;
    *(int32_t*) ((char*) r + off_timedout) = 0;
    *(void**)   ((char*) r + off_stdout)   = NULL;
    *(void**)   ((char*) r + off_stderr)   = NULL;
    return r;
}

void* __cajeta_proc_spawn(
        void* argv_arr, void* cwd_str, void* env_arr, int32_t stdio_flags,
        int64_t str_size, int64_t str_off_bytes, int64_t str_off_len,
        void* proc_vtable, int64_t proc_size,
        int64_t off_handle, int64_t off_stdin_fd,
        int64_t off_stdout_fd, int64_t off_stderr_fd) {
    (void) argv_arr; (void) cwd_str; (void) env_arr; (void) stdio_flags;
    (void) str_size; (void) str_off_bytes; (void) str_off_len;
    void* p = __cajeta_alloc((uint64_t) proc_size);
    *(void**)   ((char*) p)                 = proc_vtable;
    *(int64_t*) ((char*) p + off_handle)    = 0;
    *(int32_t*) ((char*) p + off_stdin_fd)  = -1;
    *(int32_t*) ((char*) p + off_stdout_fd) = -1;
    *(int32_t*) ((char*) p + off_stderr_fd) = -1;
    return p;
}
int32_t __cajeta_proc_pid(int64_t handle_i) { (void) handle_i; return -1; }
void __cajeta_proc_kill(int64_t handle_i) { (void) handle_i; }
void* __cajeta_proc_wait(
        int64_t handle_i, int64_t timeout_ms,
        void* result_vtable, int64_t res_size,
        int64_t off_launched, int64_t off_exited, int64_t off_exitcode,
        int64_t off_signaled, int64_t off_signal, int64_t off_timedout,
        int64_t off_stdout, int64_t off_stderr) {
    (void) handle_i; (void) timeout_ms;
    void* r = __cajeta_alloc((uint64_t) res_size);
    *(void**)   ((char*) r)                = result_vtable;
    *(int32_t*) ((char*) r + off_launched) = 0;
    *(int32_t*) ((char*) r + off_exited)   = 0;
    *(int32_t*) ((char*) r + off_exitcode) = -1;
    *(int32_t*) ((char*) r + off_signaled) = 0;
    *(int32_t*) ((char*) r + off_signal)   = 0;
    *(int32_t*) ((char*) r + off_timedout) = 0;
    *(void**)   ((char*) r + off_stdout)   = NULL;
    *(void**)   ((char*) r + off_stderr)   = NULL;
    return r;
}
void __cajeta_proc_release(int64_t handle_i) { (void) handle_i; }
#endif  // !_WIN32

// Stat helpers take (bytes, length) and NUL-terminate; predicates return 1/0, errors -1.

#include <dirent.h>

#define __CAJETA_PATH_MAX 4096

static int __cajeta_copy_path_with_nul(
        char* dst, size_t dst_size,
        const char* bytes, int64_t length) {
    if (length < 0) length = 0;
    if ((size_t) length >= dst_size) return -1;
    if (length > 0 && bytes) memcpy(dst, bytes, (size_t) length);
    dst[length] = '\0';
    return 0;
}

int32_t __cajeta_path_exists(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return 0;
    struct stat st;
    return stat(path, &st) == 0 ? 1 : 0;
}

int32_t __cajeta_path_is_file(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISREG(st.st_mode) ? 1 : 0;
}

int32_t __cajeta_path_is_dir(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int32_t __cajeta_path_is_symlink(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return 0;
    struct stat st;
    if (lstat(path, &st) != 0) return 0;   // the link itself, not its target
    return S_ISLNK(st.st_mode) ? 1 : 0;
}

// Fill the caller's FileInfo from stat(); -1 if stat fails. The struct mirror below
// MUST match FileInfo.cajeta after its 8-byte vtable slot: { i64 size, created,
// modified, accessed; i1 isFile, isDir, isSymlink; i32 perms } = 40 bytes, 48 total.
typedef struct {
    void*    vtable;
    int64_t  size;
    int64_t  createdNanos;
    int64_t  modifiedNanos;
    int64_t  accessedNanos;
    int8_t   isFile;
    int8_t   isDir;
    int8_t   isSymlink;
    int8_t   _pad;
    int32_t  permissions;
} __cajeta_FileInfoLayout;

int32_t __cajeta_path_stat(const char* path, void* fileInfo) {
    if (!path || !fileInfo) return -1;
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    __cajeta_FileInfoLayout* fi = (__cajeta_FileInfoLayout*) fileInfo;
    fi->size = (int64_t) st.st_size;
#ifdef __linux__
    fi->createdNanos =
        (int64_t) st.st_ctim.tv_sec * 1000000000LL + (int64_t) st.st_ctim.tv_nsec;
    fi->modifiedNanos =
        (int64_t) st.st_mtim.tv_sec * 1000000000LL + (int64_t) st.st_mtim.tv_nsec;
    fi->accessedNanos =
        (int64_t) st.st_atim.tv_sec * 1000000000LL + (int64_t) st.st_atim.tv_nsec;
#else
    fi->createdNanos = (int64_t) st.st_ctime * 1000000000LL;
    fi->modifiedNanos = (int64_t) st.st_mtime * 1000000000LL;
    fi->accessedNanos = (int64_t) st.st_atime * 1000000000LL;
#endif
    fi->isFile = S_ISREG(st.st_mode) ? 1 : 0;
    fi->isDir = S_ISDIR(st.st_mode) ? 1 : 0;
    // stat() followed the link, so the flag needs an lstat; the rest is the target.
    struct stat lst;
    fi->isSymlink = (lstat(path, &lst) == 0 && S_ISLNK(lst.st_mode)) ? 1 : 0;
    fi->permissions = (int32_t) (st.st_mode & 07777);
    return 0;
}

// Recursive mkdir, as `mkdir -p`; -1 when a component exists as a non-directory.
int32_t __cajeta_path_mkdirs(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return -1;
    if (path[0] == '\0') return -1;

    for (char* p = path + 1; *p != '\0'; ++p) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(path, &st) != 0) {
                if (cajeta_mkdir(path, 0777) != 0 && errno != EEXIST) {
                    *p = '/';
                    return -1;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                *p = '/';
                return -1;
            }
            *p = '/';
        }
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode) ? 0 : -1;
    }
    if (cajeta_mkdir(path, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

int32_t __cajeta_path_delete(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return -1;
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        return rmdir(path) == 0 ? 0 : -1;
    }
    return unlink(path) == 0 ? 0 : -1;
}

void* __cajeta_path_canonical(const char* bytes, int64_t length) {
    char in[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(in, sizeof(in), bytes, length) != 0) return NULL;
    char* canon = realpath(in, NULL);
    if (!canon) return NULL;
    size_t n = strlen(canon);
    void* hdr = __cajeta_new_array_header(8, 1, (uint64_t) n);
    if (!hdr) {
        free(canon);
        return NULL;
    }
    memcpy(((char*) hdr) + 8, canon, n);
    free(canon);
    return hdr;
}

// Non-recursive listing: a CajetaArray whose int8[] payload holds the child NAMES
// (no "." or ".."), byte-sorted and NUL-joined — that order is part of the contract.
static int __cajeta_name_cmp(const void* a, const void* b) {
    return strcmp(*(const char* const*) a, *(const char* const*) b);
}
void* __cajeta_path_list(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    size_t total = 0;
    size_t count = 0;
    size_t cap = 64;
    char** names = NULL;
    DIR* d = NULL;
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) == 0) {
        d = opendir(path);
    }
    if (d) {
        names = (char**) malloc(cap * sizeof(char*));
        struct dirent* de;
        while (names && (de = readdir(d)) != NULL) {
            const char* n = de->d_name;
            if (n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'))) {
                continue;  // skip "." / ".."
            }
            if (count == cap) {
                cap *= 2;
                char** grown = (char**) realloc(names, cap * sizeof(char*));
                if (!grown) break;
                names = grown;
            }
            names[count] = strdup(n);
            if (!names[count]) break;
            total += strlen(n) + 1;  // name + NUL separator
            count++;
        }
        closedir(d);
    }
    if (count > 0) {
        qsort(names, count, sizeof(char*), __cajeta_name_cmp);
    }
    void* hdr = __cajeta_new_array_header(8, 1, (uint64_t) total);
    if (hdr) {
        char* out = ((char*) hdr) + 8;
        for (size_t i = 0; i < count; i++) {
            size_t n = strlen(names[i]);
            memcpy(out, names[i], n);
            out += n;
            *out++ = '\0';
        }
    }
    for (size_t i = 0; i < count; i++) free(names[i]);
    free(names);
    return hdr;
}

// chmod a+x, preserving the other bits; a no-op success on Windows.
int32_t __cajeta_path_set_executable(const char* bytes, int64_t length) {
    char path[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(path, sizeof(path), bytes, length) != 0) return -1;
#if defined(_WIN32)
    (void) path;
    return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    mode_t mode = st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH;
    return chmod(path, mode) == 0 ? 0 : -1;
#endif
}

// symlink(target, link), removing an existing `link` so it can be re-pointed.
int32_t __cajeta_path_symlink(const char* targetBytes, int64_t targetLen,
                              const char* linkBytes, int64_t linkLen) {
    char target[__CAJETA_PATH_MAX];
    char link[__CAJETA_PATH_MAX];
    if (__cajeta_copy_path_with_nul(target, sizeof(target), targetBytes, targetLen) != 0) return -1;
    if (__cajeta_copy_path_with_nul(link, sizeof(link), linkBytes, linkLen) != 0) return -1;
#if defined(_WIN32)
    (void) target; (void) link;
    return -1;
#else
    struct stat st;
    if (lstat(link, &st) == 0) {
        unlink(link);
    }
    return symlink(target, link) == 0 ? 0 : -1;
#endif
}

// At-exit registry for @PreDestroy: atexit() dangles in JIT'd runs, so the caller fires these, LIFO.

typedef struct CajetaAtExitNode {
    void (*fn)(void*);
    void* arg;
    struct CajetaAtExitNode* next;
} CajetaAtExitNode;

static CajetaAtExitNode* __cajeta_atexit_head = NULL;

void __cajeta_atexit_push(void (*fn)(void*), void* arg) {
    if (!fn) return;
    CajetaAtExitNode* n = (CajetaAtExitNode*) malloc(sizeof(CajetaAtExitNode));
    if (!n) return;
    n->fn = fn;
    n->arg = arg;
    n->next = __cajeta_atexit_head;
    __cajeta_atexit_head = n;
}

void __cajeta_run_atexit_handlers(void) {
    CajetaAtExitNode* n = __cajeta_atexit_head;
    __cajeta_atexit_head = NULL;
    while (n) {
        CajetaAtExitNode* next = n->next;
        n->fn(n->arg);
        free(n);
        n = next;
    }
}
