// C++ collection/ sort competitors: sort-int64 (random, ascending, descending,
// dups), sort-f64, sort-stable-int64 at the page shape (1e6; n/100 under
// CAJETA_BENCH_SMOKE=1), same inputs and checks as the Cajeta benchmarks
// (CollGen): random = key(i), f64 = draw / 2^31, stable = (value, index) pairs
// packed value << 20 | index, value = draw % 1e5, sorted on the value.
// std::sort and pdqsort (the pinned orlp/pdqsort, competitors/pins.tsv) for
// the unstable rows, std::stable_sort with a value comparator for the stable
// row. The input copy is outside the time.
#include <pdqsort.h>
#include "../common/collgen.h"

using namespace collgen;
constexpr int64_t FULL = 1'000'000;

int main() {
    const int64_t n = sized(FULL);
    const std::string note = size_note(FULL);
    auto ok64 = [&](const std::vector<int64_t>& w) { return (int64_t) w.size() == n && std::is_sorted(w.begin(), w.end()); };
    if (selected("sort-int64"))
        for (const char* v : {"random", "ascending", "descending", "dups"}) {
            std::vector<int64_t> src(n);
            std::string vs = v;
            for (int64_t i = 0; i < n; i++)
                src[i] = vs == "random" ? key(i) : vs == "ascending" ? i : vs == "descending" ? n - 1 - i : i % 16;
            rowv("sort-int64", "std::sort", "libstdc++", n, bench_with([&] { return src; },
                [](std::vector<int64_t> w) { std::sort(w.begin(), w.end()); return w; }, ok64), note.c_str(), v);
            rowv("sort-int64", "pdqsort", "orlp b1ef26a", n, bench_with([&] { return src; },
                [](std::vector<int64_t> w) { pdqsort(w.begin(), w.end()); return w; }, ok64), note.c_str(), v);
        }
    if (selected("sort-f64")) {
        Gen g;
        std::vector<double> src(n);
        for (auto& x : src) x = g.next() / 2147483648.0;
        auto okf = [&](const std::vector<double>& w) { return (int64_t) w.size() == n && std::is_sorted(w.begin(), w.end()); };
        row("sort-f64", "std::sort", "libstdc++", n, bench_with([&] { return src; },
            [](std::vector<double> w) { std::sort(w.begin(), w.end()); return w; }, okf), note);
        row("sort-f64", "pdqsort", "orlp b1ef26a", n, bench_with([&] { return src; },
            [](std::vector<double> w) { pdqsort(w.begin(), w.end()); return w; }, okf), note);
    }
    if (selected("sort-stable-int64")) {
        Gen g;
        std::vector<int64_t> src(n);
        for (int64_t i = 0; i < n; i++) src[i] = ((int64_t) (g.next() % 100000) << 20) | i;
        row("sort-stable-int64", "std::stable_sort", "libstdc++", n, bench_with([&] { return src; },
            [](std::vector<int64_t> w) { std::stable_sort(w.begin(), w.end(), [](int64_t a, int64_t b) { return (a >> 20) < (b >> 20); }); return w; },
            ok64), size_note(FULL, "(value index) pairs packed in an int64; comparator on the value"));
    }
}
