// aot/ spawn timer (bench/aot/Components.md, benchmark-reference unit 13): times
// a program's whole start-up -- posix_spawn to exit -- the way the Cajeta row's
// ExternalBenchmark times the cajeta.aot graph. Each trial spawns the command
// with stdout on a pipe, drains it, waits (wait4, for the child's ru_maxrss),
// and stops the clock; the trial's stdout (trimmed) must equal --expect and the
// exit status must be 0, or the row is invalid. Prints one v1 row (31 columns,
// competitors/columns.txt order) for area aot.
//
//   spawntimer --bench SLUG --lang L --lib LIB --ver VER [--notes N] [--input N]
//              --expect LINE -- cmd args...
// Env: PROFILE_WARMUP (default 3), PROFILE_TRIALS (default 20), PROFILE_RUN_ID,
//      PROFILE_RUN_TS, PROFILE_LANG_VERSION, PROFILE_FLAGS (the row's flags).
#include <spawn.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

extern char** environ;

static std::string env(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return (v && *v) ? std::string(v) : std::string(d);
}

static std::string clean(std::string s) {  // a CSV field: no commas, quotes or newlines
    for (char& c : s) if (c == ',' || c == '"' || c == '\n' || c == '\r') c = ';';
    return s;
}

struct Run { long long ns; int status; std::string out; long maxrss_kb; bool spawned; };

static Run run_once(std::vector<char*>& argv) {
    Run r{-1, -1, "", -1, false};
    int p[2];
    if (pipe(p) != 0) return r;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_addclose(&fa, p[0]);
    posix_spawn_file_actions_addclose(&fa, p[1]);
    auto t0 = std::chrono::steady_clock::now();
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (rc != 0) { close(p[0]); return r; }
    r.spawned = true;
    char buf[4096];
    ssize_t n;
    while ((n = read(p[0], buf, sizeof buf)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; break; }
        r.out.append(buf, (size_t)n);
    }
    close(p[0]);
    int st = 0;
    struct rusage ru;
    while (wait4(pid, &st, 0, &ru) < 0 && errno == EINTR) {}
    r.ns = (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - t0).count();
    r.status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    r.maxrss_kb = ru.ru_maxrss;
    while (!r.out.empty() && (r.out.back() == '\n' || r.out.back() == '\r' || r.out.back() == ' ')) r.out.pop_back();
    return r;
}

int main(int argc, char** argv) {
    std::string bench = "di-graph-100-startup", lang, lib, ver, notes, expect;
    long long input = 100;
    int i = 1;
    for (; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--") { i++; break; }
        if (i + 1 >= argc) { std::fprintf(stderr, "spawntimer: %s needs a value\n", a.c_str()); return 2; }
        std::string v = argv[++i];
        if (a == "--bench") bench = v; else if (a == "--lang") lang = v; else if (a == "--lib") lib = v;
        else if (a == "--ver") ver = v; else if (a == "--notes") notes = v; else if (a == "--expect") expect = v;
        else if (a == "--input") input = std::atoll(v.c_str());
        else { std::fprintf(stderr, "spawntimer: unknown flag %s\n", a.c_str()); return 2; }
    }
    if (i >= argc) { std::fprintf(stderr, "spawntimer: no command\n"); return 2; }
    std::vector<char*> cmd(argv + i, argv + argc);
    cmd.push_back(nullptr);

    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::max(1, std::atoi(env("PROFILE_TRIALS", "20").c_str()));
    bool ok = true;
    std::string why;
    long maxrss = -1;
    std::vector<long long> s;
    for (int t = 0; t < warmup + trials; t++) {
        Run r = run_once(cmd);
        if (!r.spawned) { ok = false; why = std::string("could not spawn ") + cmd[0]; break; }
        if (r.status != 0) { ok = false; why = "exit status " + std::to_string(r.status); }
        else if (r.out != expect) { ok = false; why = "stdout '" + r.out + "' != expected '" + expect + "'"; }
        maxrss = std::max(maxrss, r.maxrss_kb);
        if (t >= warmup) s.push_back(r.ns);
    }
    std::string rid = env("PROFILE_RUN_ID", "local"), ts = env("PROFILE_RUN_TS", "");
    std::string lv = clean(env("PROFILE_LANG_VERSION", "")), flags = clean(env("PROFILE_FLAGS", ""));
    std::string nn = clean(notes + (why.empty() ? "" : "; " + why));
    if (s.empty()) {
        std::printf("1,%s,%s,%s,aot,,0,,%lld,%s,%s,%s,%s,%s,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,failed,false,%s,\n",
                    rid.c_str(), ts.c_str(), bench.c_str(), input, lang.c_str(), lv.c_str(), lib.c_str(),
                    clean(ver).c_str(), flags.c_str(), nn.c_str());
        return 0;
    }
    std::sort(s.begin(), s.end());
    size_t n = s.size();
    long long mean = std::accumulate(s.begin(), s.end(), 0LL) / (long long)n;
    long long p95 = s[std::min(n - 1, n * 95 / 100)];
    // throughput: start-ups per second from the median
    char thr[64];
    std::snprintf(thr, sizeof thr, "%.2f", 1e9 / (double)s[n / 2]);
    std::printf("1,%s,%s,%s,aot,,0,,%lld,%s,%s,%s,%s,%s,%d,%d,%lld,%lld,%lld,%lld,%s,starts/s,%ld,-1,-1,-1,-1,%s,%s,%s,\n",
                rid.c_str(), ts.c_str(), bench.c_str(), input, lang.c_str(), lv.c_str(), lib.c_str(),
                clean(ver).c_str(), flags.c_str(), warmup, trials, s[0], s[n / 2], mean, p95, thr, maxrss,
                ok ? "ok" : "invalid", ok ? "true" : "false", nn.c_str());
    return 0;
}
