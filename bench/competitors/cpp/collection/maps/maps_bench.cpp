// C++ collection/ competitors, hash half: hashmap-lookup-hit/-miss/-iterate,
// hashset-contains and the ImmutableMap/ImmutableSet ids, same sizes, inputs
// and checks as the Cajeta benchmarks. std::unordered_map / unordered_set and,
// for the HashMap ids, ankerl::unordered_dense::map (the pinned
// unordered_dense, competitors/pins.tsv). Immutable = a const copy behind
// shared_ptr: C++ has no frozen map or set, and those pages name none.
#include <memory>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <ankerl/unordered_dense.h>
#include "../common/collgen.h"

using namespace collgen;
constexpr int N = 1'000'000, Q = 10'000'000;
std::vector<int64_t> keys, hitQ, missQ, halfQ;
int64_t hitExp = 0, halfExp = 0;

template <class M> void mapRows(const char* lib, const char* ver) {
    M m;
    for (int i = 0; i < N; i++) m[keys[i]] = i;
    if (selected("hashmap-lookup-hit"))
        row("hashmap-lookup-hit", lib, ver, Q, bench([&] { int64_t s = 0; for (auto k : hitQ) s += m.find(k)->second; return s; },
            [](int64_t s) { return s == hitExp; }), "find");
    if (selected("hashmap-lookup-miss"))
        row("hashmap-lookup-miss", lib, ver, Q, bench([&] { int64_t c = 0; for (auto k : missQ) if (m.find(k) == m.end()) c++; return c; },
            [](int64_t c) { return c == Q; }), "find == end");
    if (selected("hashmap-iterate"))
        row("hashmap-iterate", lib, ver, Q, bench([&] { int64_t s = 0; for (int p = 0; p < 10; p++) for (auto& e : m) s += e.second; return s; },
            [](int64_t s) { return s == 10LL * ((int64_t) N * (N - 1) / 2); }), "range for");
}

// hashmap-int and hashmap-string at the page shape (n/100 under
// CAJETA_BENCH_SMOKE=1): insert into an empty map, then look up. The string
// map holds string_views of the key vector (no copy), as the Cajeta map
// borrows its keys; lookups go through a separate copy of the keys.
template <class IM, class SM> void firstSuite(const char* lib, const char* ver) {
    const int64_t n = sized(N);
    if (selected("hashmap-int")) {
        std::vector<int64_t> ks(n);
        for (int64_t i = 0; i < n; i++) ks[i] = key(i);
        row("hashmap-int", lib, ver, n, bench([&] {
            IM m;
            for (int64_t i = 0; i < n; i++) m[ks[i]] = i;
            int64_t s = 0;
            for (auto k : ks) s += m.find(k)->second;
            return s;
        }, [&](int64_t s) { return s == n * (n - 1) / 2; }), size_note(N));
    }
    if (selected("hashmap-string")) {
        std::vector<std::string> ks(n), ps(n);
        for (int64_t i = 0; i < n; i++) { ks[i] = hex16(key(i)); ps[i] = std::string(ks[i].data(), ks[i].size()); }
        row("hashmap-string", lib, ver, n, bench([&] {
            SM m;
            for (int64_t i = 0; i < n; i++) m[std::string_view(ks[i])] = i;
            int64_t s = 0;
            for (auto& p : ps) s += m.find(std::string_view(p))->second;
            return s;
        }, [&](int64_t s) { return s == n * (n - 1) / 2; }), size_note(N, "string_view keys; lookups through a separate copy"));
    }
}

int main() {
    firstSuite<std::unordered_map<int64_t, int64_t>, std::unordered_map<std::string_view, int64_t>>("std::unordered_map", "libstdc++");
    firstSuite<ankerl::unordered_dense::map<int64_t, int64_t>, ankerl::unordered_dense::map<std::string_view, int64_t>>("ankerl::unordered_dense::map", "4.8.1");
    if (selected("hashset-dedup")) {
        const int64_t nd = sized(10LL * N);
        auto vals = Gen().indices(nd, (int32_t) (nd / 10));
        std::vector<bool> seen(nd / 10);
        size_t exp = 0;
        for (auto v : vals) if (!seen[v]) { seen[v] = true; exp++; }
        row("hashset-dedup", "std::unordered_set", "libstdc++", nd, bench([&] {
            std::unordered_set<int32_t> st;
            for (auto v : vals) st.insert(v);
            return st.size();
        }, [&](size_t c) { return c == exp; }), size_note(10LL * N));
    }
    keys.resize(N);
    for (int i = 0; i < N; i++) keys[i] = key(i);
    auto hi = Gen().indices(Q, N);
    for (auto i : hi) { hitExp += i; hitQ.push_back(keys[i]); missQ.push_back(key((int64_t) N + i)); }
    auto half = Gen().indices(Q, 2 * N);
    for (auto i : half) { if (i < N) halfExp++; halfQ.push_back(key(i)); }
    mapRows<std::unordered_map<int64_t, int64_t>>("std::unordered_map", "libstdc++");
    mapRows<ankerl::unordered_dense::map<int64_t, int64_t>>("ankerl::unordered_dense::map", "4.8.1");
    std::unordered_map<int64_t, int64_t> m;
    std::unordered_set<int64_t> st;
    for (int i = 0; i < N; i++) { m[keys[i]] = i; st.insert(keys[i]); }
    const int64_t k7 = key(7);
    auto contains = [&](const std::unordered_set<int64_t>& s) { return [&] { int64_t c = 0; for (auto k : halfQ) if (s.count(k)) c++; return c; }; };
    if (selected("hashset-contains"))
        row("hashset-contains", "std::unordered_set", "libstdc++", Q, bench(contains(st), [](int64_t c) { return c == halfExp; }), "");
    if (selected("immutablemap-build"))
        row("immutablemap-build", "shared_ptr<const unordered_map>", "libstdc++", 10LL * N, bench([&] {
            size_t t = 0; int64_t p = 0;
            for (int r = 0; r < 10; r++) { auto f = std::make_shared<const std::unordered_map<int64_t, int64_t>>(m); t += f->size(); p = f->at(k7); }
            return t == (size_t) 10 * N && p == 7;
        }, [](bool r) { return r; }), "a const copy behind shared_ptr");
    if (selected("immutablemap-lookup")) {
        auto f = std::make_shared<const std::unordered_map<int64_t, int64_t>>(m);
        row("immutablemap-lookup", "shared_ptr<const unordered_map>", "libstdc++", Q, bench([&] { int64_t s = 0; for (auto k : hitQ) s += f->find(k)->second; return s; },
            [](int64_t s) { return s == hitExp; }), "find");
    }
    if (selected("immutableset-build"))
        row("immutableset-build", "shared_ptr<const unordered_set>", "libstdc++", 10LL * N, bench([&] {
            size_t t = 0; bool p = false;
            for (int r = 0; r < 10; r++) { auto f = std::make_shared<const std::unordered_set<int64_t>>(st); t += f->size(); p = f->count(k7) == 1; }
            return t == (size_t) 10 * N && p;
        }, [](bool r) { return r; }), "a const copy behind shared_ptr");
    if (selected("immutableset-contains")) {
        auto f = std::make_shared<const std::unordered_set<int64_t>>(st);
        row("immutableset-contains", "shared_ptr<const unordered_set>", "libstdc++", Q, bench(contains(*f), [](int64_t c) { return c == halfExp; }), "");
    }
}
