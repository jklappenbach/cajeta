// process/ competitor in C++ (bench/process/Command.md, Process.md): POSIX
// posix_spawn + waitpid, the calls the Cajeta runtime itself makes.
//   command-spawn-wait   spawn and wait /bin/true R = 1e3 times (smoke 1e2); check: every exit code 0
//   process-pipe-100mb   100 MiB (smoke 10 MiB) of data[i] = (i*31+7) & 0xFF through `cat`: a writer
//                        thread feeds stdin while the main thread reads stdout to EOF; check: length
//                        and the sum of unsigned bytes equal the input's.
// Rows are v1 (31 columns); competitors/lib/rows.sh lifts them to v2.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gnu/libc-version.h>
#include <numeric>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

static std::string env(const char* k, const char* d) { const char* v = getenv(k); return (v && *v) ? v : d; }
static int env_int(const char* k, int d) { const char* v = getenv(k); return (v && *v) ? atoi(v) : d; }
static bool smoke() { std::string s = env("CAJETA_BENCH_SMOKE", ""); return s == "1" || s == "true" || s == "yes"; }
static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status");
    std::string l;
    while (std::getline(s, l))
        if (l.rfind("VmHWM:", 0) == 0) { long long kb = -1; sscanf(l.c_str(), "VmHWM: %lld", &kb); return kb; }
    return -1;
}
struct Stats { long long mn, med, mean, p95; };
template <class F> Stats measure(int warmup, int trials, F f) {
    for (int i = 0; i < warmup; i++) f();
    std::vector<long long> v;
    for (int i = 0; i < trials; i++) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        v.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    return {v[0], v[n / 2], std::accumulate(v.begin(), v.end(), 0LL) / (long long)n, v[std::min(n - 1, n * 95 / 100)]};
}
static void emit(const char* slug, long long input, const char* lib, int w, int t, const Stats& s, bool ok,
                 const std::string& notes, bool mbps) {
    std::string thr, unit;
    if (mbps && s.med > 0) {
        char b[64];
        snprintf(b, sizeof b, "%.1f", (double)input / s.med * 1e9 / 1048576.0);
        thr = b;
        unit = "MB/s";
    }
    printf("1,%s,%s,%s,process,,0,,%lld,cpp,%s,%s,glibc %s,-O3 -march=native -DNDEBUG,%d,%d,%lld,%lld,%lld,%lld,%s,%s,%lld,-1,-1,-1,-1,%s,%s,%s,\n",
           env("PROFILE_RUN_ID", "local").c_str(), env("PROFILE_RUN_TS", "").c_str(), slug, input,
           env("PROFILE_LANG_VERSION", "").c_str(), lib, gnu_get_libc_version(), w, t, s.mn, s.med, s.mean, s.p95,
           thr.c_str(), unit.c_str(), peak_rss_kb(), ok ? "ok" : "invalid", ok ? "true" : "false", notes.c_str());
    fflush(stdout);
}

static volatile long long g_sink = 0;

int main() {
    int warmup = env_int("PROFILE_WARMUP", 1), trials = std::max(1, env_int("PROFILE_TRIALS", 5));
    bool sm = smoke();

    // ---- command-spawn-wait ----
    const int R = sm ? 100 : 1000;
    long long zero = 0;
    auto st = measure(warmup, trials, [&] {
        long long z = 0;
        char* argv[] = {(char*)"/bin/true", nullptr};
        for (int i = 0; i < R; i++) {
            pid_t pid;
            if (posix_spawn(&pid, "/bin/true", nullptr, nullptr, argv, environ) != 0) continue;
            int status = 0;
            while (waitpid(pid, &status, 0) < 0) {}
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) z++;
        }
        zero = z;
        g_sink += z;
    });
    std::string n1 = "posix_spawn(/bin/true) + waitpid x" + std::to_string(R) + " per trial; check: zero exits == " + std::to_string(R);
    if (sm) n1 += "; smoke size 1e2 spawns in place of 1e3";
    emit("command-spawn-wait", R, "posix_spawn", warmup, trials, st, zero == R, n1, false);

    // ---- process-pipe-100mb ----
    const size_t N = sm ? (10u << 20) : (100u << 20);
    std::vector<unsigned char> data(N);
    long long want = 0;
    for (size_t i = 0; i < N; i++) { data[i] = (unsigned char)((i * 31 + 7) & 0xFF); want += data[i]; }
    long long gotSum = -1;
    size_t gotLen = 0;
    auto sp = measure(warmup, trials, [&] {
        int in[2], out[2];
        if (pipe(in) || pipe(out)) { gotLen = 0; return; }
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        posix_spawn_file_actions_adddup2(&fa, out[1], 1);
        posix_spawn_file_actions_addclose(&fa, in[0]);
        posix_spawn_file_actions_addclose(&fa, in[1]);
        posix_spawn_file_actions_addclose(&fa, out[0]);
        posix_spawn_file_actions_addclose(&fa, out[1]);
        char* argv[] = {(char*)"cat", nullptr};
        pid_t pid;
        int rc = posix_spawnp(&pid, "cat", &fa, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&fa);
        close(in[0]);
        close(out[1]);
        if (rc != 0) { close(in[1]); close(out[0]); gotLen = 0; return; }
        std::thread wr([&] {
            size_t off = 0;
            while (off < N) {
                ssize_t k = write(in[1], data.data() + off, std::min<size_t>(N - off, 1 << 16));
                if (k <= 0) break;
                off += (size_t)k;
            }
            close(in[1]);
        });
        std::vector<unsigned char> buf(N + 65536);
        size_t len = 0;
        for (;;) {
            ssize_t k = read(out[0], buf.data() + len, buf.size() - len);
            if (k <= 0) break;
            len += (size_t)k;
            if (len == buf.size()) buf.resize(buf.size() * 2);
        }
        close(out[0]);
        wr.join();
        int status;
        while (waitpid(pid, &status, 0) < 0) {}
        long long s = 0;
        for (size_t i = 0; i < len; i++) s += buf[i];
        gotSum = s;
        gotLen = len;
        g_sink += s;
    });
    std::string n2 = "posix_spawnp(cat) with stdin/stdout pipes; writer thread (64 KiB writes) + reader to EOF; check: length " +
                     std::to_string(N) + " and byte sum " + std::to_string(want);
    if (sm) n2 += "; smoke size 10 MiB in place of 100 MiB";
    emit("process-pipe-100mb", (long long)N, "posix_spawn", warmup, trials, sp, gotLen == N && gotSum == want, n2, true);
    return 0;
}
