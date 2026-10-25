// C++ codec competitors — parse the nativejson corpus to a DOM and emit one
// schema-conformant CSV row per (file, library), matching competitors/columns.txt.
// Libraries: simdjson (SIMD SOTA DOM) and yyjson (fastest portable C DOM).
// Cross-check: a known element count per file (twitter.statuses / citm.events /
// canada.features). Built directly from the vendored single-file amalgamations.
#include "simdjson.h"
#include "yyjson.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

using clock_type = std::chrono::steady_clock;

static std::string env(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(d);
}

#include <atomic>
#include <cstdlib>
#include <new>
static std::atomic<unsigned long long> _ALLOCED{0};
static std::atomic<unsigned long long> _LASTALLOC{0};
void* operator new(std::size_t n) { _ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void* operator new[](std::size_t n) { _ALLOCED += n; void* p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
template <class F> static void _alloc_of(F run) { _ALLOCED = 0; run(); _LASTALLOC = _ALLOCED.load(); }
static unsigned long long _la() { return _LASTALLOC.load(); }

static long long peak_rss_kb() {
    std::ifstream s("/proc/self/status");
    std::string line;
    while (std::getline(s, line)) {
        if (line.rfind("VmHWM:", 0) == 0) {
            long long kb = -1;
            std::sscanf(line.c_str(), "VmHWM: %lld", &kb);
            return kb;
        }
    }
    return -1;
}

struct Stats { long long mn, med, mean, p95; };

static Stats stats(std::vector<long long> v) {
    std::sort(v.begin(), v.end());
    size_t n = v.size();
    long long sum = std::accumulate(v.begin(), v.end(), 0LL);
    return Stats{ v.front(), v[n / 2], sum / (long long)n, v[std::min(n - 1, n * 95 / 100)] };
}

static void emit(const std::string& run_id, const std::string& ts, const char* dataset,
                 size_t bytes, const char* lib, const char* ver, int warmup, int trials,
                 const Stats& s, bool check_ok, const char* status) {
    double mbps = s.med > 0 ? (double)bytes / (double)s.med * 1e9 / 1048576.0 : 0.0;
    std::printf(
        "1,%s,%s,json-dom,codec,%s,%zu,,%zu,cpp,%s,%s,%s,-O3 -march=native,%d,%d,"
        "%lld,%lld,%lld,%lld,%.1f,MB/s,%lld,-1,%llu,-1,-1,%s,%s,,\n",
        run_id.c_str(), ts.c_str(), dataset, bytes, bytes,
        env("PROFILE_LANG_VERSION", "").c_str(), lib, ver, warmup, trials,
        s.mn, s.med, s.mean, s.p95, mbps, peak_rss_kb(), (unsigned long long)_la(), status,
        check_ok ? "true" : "false");
}

struct Corpus { const char* dataset; const char* fname; size_t expect; };
static const Corpus CORPUS[] = {
    {"twitter", "twitter.json", 100},
    {"citm_catalog", "citm_catalog.json", 184},
    {"canada", "canada.json", 1},
};

// ---- simdjson ----
static size_t simdjson_count(simdjson::dom::element doc, const char* dataset) {
    using namespace simdjson;
    if (std::string(dataset) == "citm_catalog") {
        dom::object o;
        if (doc["events"].get(o)) return 0;
        return o.size();
    }
    const char* key = std::string(dataset) == "twitter" ? "statuses" : "features";
    dom::array a;
    if (doc[key].get(a)) return 0;
    return a.size();
}

// ---- yyjson ----
static size_t yyjson_count(yyjson_val* root, const char* dataset) {
    if (std::string(dataset) == "citm_catalog") {
        yyjson_val* e = yyjson_obj_get(root, "events");
        return e ? yyjson_obj_size(e) : 0;
    }
    const char* key = std::string(dataset) == "twitter" ? "statuses" : "features";
    yyjson_val* a = yyjson_obj_get(root, key);
    return a ? yyjson_arr_size(a) : 0;
}

int main() {
    const char* dd = std::getenv("PROFILE_DATA_DIR");
    if (!dd) { std::fprintf(stderr, "cpp/codec: PROFILE_DATA_DIR unset — skipping\n"); return 0; }
    std::string data_dir(dd);
    std::string run_id = env("PROFILE_RUN_ID", "local");
    std::string ts = env("PROFILE_RUN_TS", "");
    int warmup = std::atoi(env("PROFILE_WARMUP", "3").c_str());
    int trials = std::atoi(env("PROFILE_TRIALS", "10").c_str());

    for (const auto& c : CORPUS) {
        std::string path = data_dir + "/" + c.fname;
        std::ifstream in(path, std::ios::binary);
        if (!in) continue; // dataset absent → skip
        std::ostringstream ss; ss << in.rdbuf();
        std::string raw = ss.str();
        size_t n = raw.size();

        // simdjson — reuse a padded_string + parser across trials (its intended use)
        {
            simdjson::dom::parser parser;
            simdjson::padded_string ps(raw);
            bool check = false;
            auto r = parser.parse(ps);
            if (!r.error()) check = simdjson_count(r.value(), c.dataset) == c.expect;
            _alloc_of([&]{ simdjson::dom::parser p2; auto e2 = p2.parse(ps); (void)e2; });
            for (int i = 0; i < warmup; ++i) { auto e = parser.parse(ps); (void)e; }
            std::vector<long long> samples;
            for (int i = 0; i < trials; ++i) {
                auto t0 = clock_type::now();
                auto doc = parser.parse(ps);
                auto t1 = clock_type::now();
                volatile bool sink = doc.error(); (void)sink;
                samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
            emit(run_id, ts, c.dataset, n, "simdjson", SIMDJSON_VERSION,
                 warmup, trials, stats(samples), check, check ? "ok" : "invalid");
        }

        // yyjson — read + free each trial (parse owns the doc)
        {
            bool check = false;
            yyjson_doc* d0 = yyjson_read(raw.data(), n, 0);
            if (d0) { check = yyjson_count(yyjson_doc_get_root(d0), c.dataset) == c.expect; yyjson_doc_free(d0); }
            for (int i = 0; i < warmup; ++i) { yyjson_doc* d = yyjson_read(raw.data(), n, 0); yyjson_doc_free(d); }
            std::vector<long long> samples;
            for (int i = 0; i < trials; ++i) {
                auto t0 = clock_type::now();
                yyjson_doc* d = yyjson_read(raw.data(), n, 0);
                auto t1 = clock_type::now();
                volatile bool sink = (d != nullptr); (void)sink;
                yyjson_doc_free(d);
                samples.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            }
            emit(run_id, ts, c.dataset, n, "yyjson", "0.10", warmup, trials,
                 stats(samples), check, check ? "ok" : "invalid");
        }
    }
    return 0;
}
