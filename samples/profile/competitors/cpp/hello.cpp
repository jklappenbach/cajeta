// C++ "hello-metric" runner — emits one schema-conformant CSV row (columns.txt
// order). Real per-area C++ runners (simdjson/yyjson/abseil/Highway via
// CMake) extend this contract in Units 5+.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

int main() {
    const char* run_id = std::getenv("PROFILE_RUN_ID"); if (!run_id) run_id = "local";
    const char* ts     = std::getenv("PROFILE_RUN_TS"); if (!ts) ts = "";
    const char* ver    = std::getenv("PROFILE_LANG_VERSION"); if (!ver) ver = "";

    auto t0 = std::chrono::steady_clock::now();
    uint64_t acc = 0;
    for (uint64_t i = 0; i < 1000000ULL; ++i) acc += i;
    auto t1 = std::chrono::steady_clock::now();
    volatile uint64_t sink = acc; (void) sink;

    long long ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    std::printf(
        "1,%s,%s,hello,scaffold,,-1,,1000000,cpp,%s,stdlib,,-O2,0,1,%lld,%lld,%lld,%lld,,,-1,-1,-1,-1,-1,ok,true,,\n",
        run_id, ts, ver, ns, ns, ns, ns);
    return 0;
}
