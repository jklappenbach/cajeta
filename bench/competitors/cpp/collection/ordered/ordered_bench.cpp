// C++ collection/ competitors, ordered half: redblacktree-insert-lookup (the
// Cajeta shape), redblacktree-range, bplustree-insert, bplustree-point-lookup,
// bplustree-range, same sizes, inputs and checks as the Cajeta benchmarks.
// std::map (a red-black tree; the page's C++ competitor for both pages).
#include <map>
#include "../common/collgen.h"

using namespace collgen;
constexpr int N = 1'000'000;

int main() {
    if (selected("redblacktree-insert-lookup")) {
        const int64_t m = sized(N);
        std::vector<int64_t> ks(m);
        for (int64_t i = 0; i < m; i++) ks[i] = key(i);
        row("redblacktree-insert-lookup", "std::map", "libstdc++", m, bench([&] {
            std::map<int64_t, int64_t> t;
            for (int64_t i = 0; i < m; i++) t.emplace(ks[i], i);
            int64_t s = 0;
            for (auto k : ks) s += t.find(k)->second;
            return s;
        }, [&](int64_t s) { return s == m * (m - 1) / 2; }), size_note(N));
    }
    std::vector<int64_t> keys(N);
    for (int i = 0; i < N; i++) keys[i] = key(i);
    if (selected("bplustree-insert"))
        row("bplustree-insert", "std::map", "libstdc++", N, bench([&] {
            std::map<int64_t, int64_t> t;
            for (int i = 0; i < N; i++) t.emplace(keys[i], i);
            return t.size() == (size_t) N && t.at(keys[7]) == 7;
        }, [](bool r) { return r; }), "");
    std::map<int64_t, int64_t> tree;
    for (int i = 0; i < N; i++) tree.emplace(keys[i], i);
    if (selected("bplustree-point-lookup")) {
        auto idx = Gen().indices(10'000'000, N);
        int64_t exp = 0;
        std::vector<int64_t> q;
        q.reserve(idx.size());
        for (auto i : idx) { exp += i; q.push_back(keys[i]); }
        row("bplustree-point-lookup", "std::map", "libstdc++", 10'000'000, bench([&] {
            int64_t s = 0;
            for (auto k : q) s += tree.find(k)->second;
            return s;
        }, [&](int64_t s) { return s == exp; }), "find");
    }
    // Ranges: 1e4 scans of 1e3 entries from the first key >= key(draw % 1e6).
    std::vector<int64_t> starts;
    for (auto i : Gen().indices(10'000, N)) starts.push_back(key(i));
    std::vector<std::pair<int64_t, int64_t>> srt;
    for (int i = 0; i < N; i++) srt.push_back({keys[i], i});
    std::sort(srt.begin(), srt.end());
    int64_t rangeExp = 0;
    for (auto s : starts) {
        auto p = std::lower_bound(srt.begin(), srt.end(), std::make_pair(s, INT64_MIN)) - srt.begin();
        for (auto j = p; j < std::min<int64_t>(N, p + 1000); j++) rangeExp += srt[j].second;
    }
    for (const char* id : {"redblacktree-range", "bplustree-range"})
        if (selected(id))
            row(id, "std::map", "libstdc++", 10'000'000, bench([&] {
                int64_t s = 0;
                for (auto st : starts) {
                    int c = 0;
                    for (auto it = tree.lower_bound(st); it != tree.end() && c < 1000; ++it, ++c) s += it->second;
                }
                return s;
            }, [&](int64_t s) { return s == rangeExp; }), "lower_bound then 1000 increments");
}
