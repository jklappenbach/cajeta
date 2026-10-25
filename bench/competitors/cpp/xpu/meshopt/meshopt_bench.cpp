// xpu/mesh-simplify competitor: meshoptimizer's meshopt_simplify (v0.25,
// pinned in pins.tsv, built from source with the program) on the same grid
// height field as the Cajeta row (bench/xpu/MeshSimplifyBench.cajeta):
// z = 0.1 sin(3 pi x) cos(3 pi y) over the unit square, two triangles per cell,
// simplified to a tenth of its triangles. Variants: the measured size
// (19800 -> 1980, or 2048 -> 204 under CAJETA_BENCH_SMOKE=1) and the page's
// 1e6 -> 1e5 (707 x 707 cells, 999698 -> 99969), which the Cajeta row skips.
// target_error 1.0 (no error cap, so the count is what stops it), options 0.
// Check: survivors in (0, target], none degenerate, every used vertex within
// 0.02 of the surface (meshopt keeps original vertices, so this holds by
// construction; it is checked anyway). Prints v1 rows (rows.sh makes them v2).
#include "meshoptimizer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static double height(double x, double y) { return 0.1 * std::sin(3.0 * M_PI * x) * std::cos(3.0 * M_PI * y); }

int main() {
    const char* smoke = std::getenv("CAJETA_BENCH_SMOKE");
    bool sm = smoke && (std::strcmp(smoke, "1") == 0 || std::strcmp(smoke, "true") == 0);
    int warm = std::getenv("PROFILE_WARMUP") ? std::atoi(std::getenv("PROFILE_WARMUP")) : 1;
    int trials = std::getenv("PROFILE_TRIALS") ? std::atoi(std::getenv("PROFILE_TRIALS")) : 5;
    const char* rid = std::getenv("PROFILE_RUN_ID"); const char* ts = std::getenv("PROFILE_RUN_TS");
    struct V { const char* label; int cells; } vs[] = {{"1e6-to-1e5", 707}, {sm ? "2048-to-204" : "19800-to-1980", sm ? 32 : 100}};
    for (auto& v : vs) {
        int c = v.cells, n = c + 1;
        std::vector<float> pos((size_t) n * n * 3);
        for (int j = 0; j < n; j++) for (int i = 0; i < n; i++) {
            size_t o = ((size_t) j * n + i) * 3; float x = (float) i / c, y = (float) j / c;
            pos[o] = x; pos[o + 1] = y; pos[o + 2] = (float) height(x, y);
        }
        std::vector<unsigned> idx; idx.reserve((size_t) c * c * 6);
        for (int j = 0; j < c; j++) for (int i = 0; i < c; i++) {
            unsigned v0 = j * n + i, v1 = v0 + 1, v2 = v0 + n, v3 = v2 + 1;
            unsigned t[6] = {v0, v1, v3, v0, v3, v2}; idx.insert(idx.end(), t, t + 6);
        }
        size_t tris = (size_t) 2 * c * c, target = tris / 10;
        std::vector<unsigned> out(idx.size());
        size_t got = 0;
        auto once = [&]() {
            float err = 0;
            got = meshopt_simplify(out.data(), idx.data(), idx.size(), pos.data(), pos.size() / 3, 12, target * 3, 1.0f, 0, &err);
        };
        for (int w = 0; w < warm; w++) once();
        std::vector<long long> s;
        for (int t = 0; t < trials; t++) {
            auto t0 = std::chrono::steady_clock::now(); once();
            s.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
        }
        bool ok = got > 0 && got / 3 <= target && got % 3 == 0;
        for (size_t k = 0; ok && k < got; k += 3) if (out[k] == out[k + 1] || out[k + 1] == out[k + 2] || out[k] == out[k + 2]) ok = false;
        for (size_t k = 0; ok && k < got; k++) {
            const float* p = &pos[(size_t) out[k] * 3];
            if (std::fabs(p[2] - height(p[0], p[1])) > 0.02) ok = false;
        }
        std::sort(s.begin(), s.end()); long long sum = 0; for (auto x : s) sum += x;
        long long med = s[s.size() / 2];
        std::printf("1,%s,%s,mesh-simplify,xpu,,-1,,%zu,cpp,%d.%d,meshoptimizer,%d.%d,-O3 -march=native,%d,%d,%lld,%lld,%lld,%lld,%.3f,triangles/s,-1,-1,-1,-1,-1,%s,%s,meshopt_simplify %zu -> %zu triangles (target %zu) target_error 1.0,%s,cpu\n",
            rid ? rid : "local", ts ? ts : "", tris, MESHOPTIMIZER_VERSION / 1000, (MESHOPTIMIZER_VERSION % 1000) / 10,
            MESHOPTIMIZER_VERSION / 1000, (MESHOPTIMIZER_VERSION % 1000) / 10, warm, trials,
            s[0], med, sum / (long long) s.size(), s[std::min(s.size() - 1, s.size() * 95 / 100)],
            med > 0 ? (double) tris * 1e9 / med : 0.0, ok ? "ok" : "invalid", ok ? "true" : "false", tris, got / 3, target, v.label);
    }
    return 0;
}
