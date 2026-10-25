// C++ collection/ competitors, sequence half: the ArrayList, LinkedList, Heap,
// ImmutableList, Collectors and binary-search ids, same sizes, inputs and
// checks as the Cajeta benchmarks (bench/src/main/cajeta/bench/collection/).
// The standard library only: vector, list, priority_queue (greater, a
// min-heap), shared_ptr<const vector> (C++ has no immutable list; the
// ImmutableList page names none), copy into a back_inserter (the Collectors
// page names none), unordered_map of vectors, lower_bound.
#include <functional>
#include <iterator>
#include <list>
#include <memory>
#include <numeric>
#include <queue>
#include <unordered_map>
#include "../common/collgen.h"

using namespace collgen;
constexpr int N1M = 1'000'000;

int main() {
    std::vector<int32_t> lst1m(N1M);
    std::iota(lst1m.begin(), lst1m.end(), 0);
    auto idx = Gen().indices(N1M, N1M);
    int64_t exp = 0;
    for (auto x : idx) exp += x;
    exp *= 100;
    auto getLoop = [&](const std::vector<int32_t>& l) {
        return [&] { int64_t s = 0; for (int p = 0; p < 100; p++) for (auto j : idx) s += l[j]; return s; };
    };
    if (selected("arraylist-get")) row("arraylist-get", "std::vector", "libstdc++", 100'000'000, bench(getLoop(lst1m), [&](int64_t s) { return s == exp; }), "operator[] (unchecked)");
    std::vector<int32_t> lst1e7(10'000'000);
    std::iota(lst1e7.begin(), lst1e7.end(), 0);
    if (selected("arraylist-iterate"))
        row("arraylist-iterate", "std::vector", "libstdc++", 100'000'000, bench([&] {
            int64_t s = 0;
            for (int p = 0; p < 10; p++) for (int32_t v : lst1e7) s += v;
            return s;
        }, [](int64_t s) { return s == 10LL * (10'000'000LL * 9'999'999LL / 2); }), "range for");
    if (selected("arraylist-sort")) {
        Gen g;
        std::vector<int32_t> src(N1M);
        for (auto& x : src) x = g.next();
        row("arraylist-sort", "std::sort", "libstdc++", N1M, bench([&] { auto a = src; std::sort(a.begin(), a.end()); return a; },
            [](const std::vector<int32_t>& a) { return a.size() == (size_t) N1M && std::is_sorted(a.begin(), a.end()); }), "includes the copy");
    }
    if (selected("arraylist-append")) {
        const int64_t n = sized(10'000'000);
        row("arraylist-append", "std::vector", "libstdc++", n, bench([&] {
            std::vector<int32_t> a;
            for (int64_t i = 0; i < n; i++) a.push_back((int32_t) i);
            return a;
        }, [&](const std::vector<int32_t>& a) { return (int64_t) a.size() == n && a[n - 1] == n - 1; }), size_note(10'000'000, "push_back without reserve"));
    }
    if (selected("linkedlist-insert-traverse")) {
        const int32_t n = (int32_t) sized(1'000'000);
        row("linkedlist-insert-traverse", "std::list", "libstdc++", n, bench([&] {
            std::list<int32_t> l;
            for (int32_t i = 0; i < n; i++) l.push_front(i);
            bool f = std::find(l.begin(), l.end(), -1) != l.end() || std::find(l.begin(), l.end(), -1) != l.end();
            return l.size() == (size_t) n && !f && l.front() == n - 1 && l.back() == 0;
        }, [](bool r) { return r; }), size_note(1'000'000, "traversal = find of an absent value; destructor in the time"));
    }
    if (selected("heap-sort")) {
        const int64_t n = sized(1'000'000);
        Gen g;
        std::vector<int32_t> vals(n);
        for (auto& x : vals) x = g.next();
        row("heap-sort", "std::priority_queue", "libstdc++", n, bench([&] {
            std::priority_queue<int32_t, std::vector<int32_t>, std::greater<int32_t>> h;
            for (auto v : vals) h.push(v);
            std::vector<int32_t> out;
            out.reserve(n);
            while (!h.empty()) { out.push_back(h.top()); h.pop(); }
            return out;
        }, [&](const std::vector<int32_t>& a) { return (int64_t) a.size() == n && std::is_sorted(a.begin(), a.end()); }), size_note(1'000'000, "greater: a min-heap"));
    }
    if (selected("heap-push-pop")) {
        Gen g;
        std::vector<int32_t> vals(10'001'000);
        for (auto& x : vals) x = g.next();
        row("heap-push-pop", "std::priority_queue", "libstdc++", 10'000'000, bench([&] {
            std::priority_queue<int32_t, std::vector<int32_t>, std::greater<int32_t>> h(std::greater<int32_t>(), std::vector<int32_t>(vals.begin(), vals.begin() + 1000));
            int64_t c = 0;
            for (size_t i = 1000; i < vals.size(); i++) { h.push(vals[i]); c += h.top(); h.pop(); }
            return c;
        }, [](int64_t c) { return c == 10738394301026345LL; }), "greater: a min-heap; push then pop");
    }
    if (selected("immutablelist-build"))
        row("immutablelist-build", "shared_ptr<const vector>", "libstdc++", 100'000'000, bench([&] {
            size_t t = 0;
            int32_t tail = 0;
            for (int r = 0; r < 100; r++) { auto f = std::make_shared<const std::vector<int32_t>>(lst1m); t += f->size(); tail = (*f)[999'999]; }
            return t == 100'000'000 && tail == 999'999;
        }, [](bool r) { return r; }), "C++ has no immutable list: a const copy behind shared_ptr");
    if (selected("immutablelist-get")) {
        auto frozen = std::make_shared<const std::vector<int32_t>>(lst1m);
        row("immutablelist-get", "shared_ptr<const vector>", "libstdc++", 100'000'000, bench(getLoop(*frozen), [&](int64_t s) { return s == exp; }), "operator[] (unchecked)");
    }
    if (selected("collectors-to-list"))
        row("collectors-to-list", "std::copy back_inserter", "libstdc++", 10'000'000, bench([&] {
            std::vector<int32_t> o;
            std::copy(lst1e7.begin(), lst1e7.end(), std::back_inserter(o));
            return o;
        }, [](const std::vector<int32_t>& o) { return o.size() == 10'000'000 && o[9'999'999] == 9'999'999; }), "no reserve (a collector grows)");
    if (selected("collectors-group-by"))
        row("collectors-group-by", "unordered_map<int32 vector>", "libstdc++", 10'000'000, bench([&] {
            std::unordered_map<int32_t, std::vector<int32_t>> g;
            for (int32_t x : lst1e7) g[x % 1000].push_back(x);
            return g.size() == 1000 && g[0].size() == 10000;
        }, [](bool r) { return r; }), "");
    if (selected("binary-search")) {
        const int64_t n = sized(10'000'000), m = n / 10;
        std::vector<int64_t> a(m), q;
        for (int64_t i = 0; i < m; i++) a[i] = i * 2;
        for (auto i : Gen().indices(n, (int32_t) m)) q.push_back(2 * (int64_t) i);
        row("binary-search", "std::lower_bound", "libstdc++", n, bench([&] {
            int64_t hits = 0;
            for (auto k : q) { auto it = std::lower_bound(a.begin(), a.end(), k); if (it != a.end() && *it == k) hits++; }
            return hits;
        }, [&](int64_t h) { return h == n; }), size_note(10'000'000));
    }
}
