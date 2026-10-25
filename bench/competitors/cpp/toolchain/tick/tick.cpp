// toolchain/debugger-attach debuggee (bench/toolchain/Debugger.md): a running
// process that calls tick() every millisecond, for gdb to attach to and break in.
// PR_SET_PTRACER_ANY lets a debugger that is not our parent attach under
// Yama ptrace_scope=1 (Ubuntu's default) without sudo.
#include <sys/prctl.h>
#include <cstdio>
#include <ctime>

extern "C" __attribute__((noinline)) long tick(long n) {
    return n * 3 + 1;
}

int main() {
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);
    std::printf("ready\n");
    std::fflush(stdout);
    volatile long acc = 0;
    timespec ms{0, 1000000};
    for (long i = 0;; i++) {
        acc = acc + tick(i);
        nanosleep(&ms, nullptr);
    }
}
