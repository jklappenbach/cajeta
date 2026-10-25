// C++ collection/ competitors for the Cache ids. The Cache page names no C++
// library and the standard library has no cache, so this is the canonical
// hand-written LRU: std::list of (key, value) in recency order plus an
// std::unordered_map from key to list iterator; get splices the hit to the
// front, put evicts the back past capacity. Same sizes, inputs and checks as
// the Cajeta benchmarks. No TTL: cache-ttl is skipped (the page names
// Caffeine, moka and ristretto for it).
#include <list>
#include <optional>
#include <unordered_map>
#include "../common/collgen.h"

using namespace collgen;
constexpr int C = 100'000, Q = 10'000'000;

struct Lru {
    size_t cap;
    std::list<std::pair<int64_t, int64_t>> order;
    std::unordered_map<int64_t, std::list<std::pair<int64_t, int64_t>>::iterator> index;
    explicit Lru(size_t c) : cap(c) { index.reserve(c + 1); }
    std::optional<int64_t> get(int64_t k) {
        auto it = index.find(k);
        if (it == index.end()) return std::nullopt;
        order.splice(order.begin(), order, it->second);
        return it->second->second;
    }
    void put(int64_t k, int64_t v) {
        auto it = index.find(k);
        if (it != index.end()) { it->second->second = v; order.splice(order.begin(), order, it->second); return; }
        order.emplace_front(k, v);
        index.emplace(k, order.begin());
        if (index.size() > cap) { index.erase(order.back().first); order.pop_back(); }
    }
    size_t size() const { return index.size(); }
};

int main() {
    const char* lib = "hand-written LRU (std::list + unordered_map)";
    std::vector<int64_t> keys(Q);
    for (int i = 0; i < Q; i++) keys[i] = key(i);
    if (selected("cache-hit")) {
        Lru hot(C);
        for (int i = 0; i < C; i++) hot.put(keys[i], i);
        std::vector<int64_t> q;
        for (auto i : Gen().indices(Q, C)) q.push_back(keys[i]);
        row("cache-hit", lib, "libstdc++", Q, bench([&] { int64_t h = 0; for (auto k : q) if (hot.get(k)) h++; return h; },
            [](int64_t h) { return h == Q; }), "no C++ cache library on the page");
    }
    if (selected("cache-miss"))
        row("cache-miss", lib, "libstdc++", Q, bench([&] {
            Lru c(C);
            int64_t m = 0;
            for (int i = 0; i < Q; i++) if (!c.get(keys[i])) { m++; c.put(keys[i], i); }
            return m == Q && c.size() == (size_t) C;
        }, [](bool r) { return r; }), "no C++ cache library on the page");
    if (selected("cache-evict"))
        row("cache-evict", lib, "libstdc++", Q, bench([&] {
            Lru c(C);
            for (int i = 0; i < Q; i++) c.put(keys[i], i);
            return c.size();
        }, [](size_t n) { return n == (size_t) C; }), "no C++ cache library on the page");
    if (selected("cache-ttl")) skip("cache-ttl", lib, 1'000'000, "no TTL in the hand-written LRU (the page names Caffeine moka and ristretto)");
}
