// Rust collection/ competitors for the Cache ids, same sizes, inputs and
// checks as the Cajeta benchmarks (capacity 1e5; 1e7 operations; TTL 1 ms over
// 1e6 entries with a 2 ms sleep before the read-back).
//   moka 0.12 sync::Cache: TinyLFU admission + LRU eviction; maintenance runs
//     in batches, so each size check follows run_pending_tasks() (in the time).
//     Under admission a miss's insert may be rejected rather than evict, so the
//     size check is the capacity, not the last 1e5 keys.
//   lru 0.18 LruCache: exact LRU; no TTL, so cache-ttl is skipped for it.
#[path = "../../common/collgen.rs"]
mod collgen;
use collgen::*;
use std::hint::black_box;
use std::num::NonZeroUsize;
use std::time::Duration;

const C: usize = 100_000;
const Q: usize = 10_000_000;

fn main() {
    let hit_q: Vec<i64> = Gen::new().indices(Q, C as i32).iter().map(|&i| key(i as i64)).collect();
    let keys: Vec<i64> = (0..Q as i64).map(key).collect();

    // ---- moka ----
    {
        let lib = "moka::sync::Cache";
        let ver = "0.12.16";
        if selected("cache-hit") {
            let c: moka::sync::Cache<i64, i64> = moka::sync::Cache::new(C as u64);
            for i in 0..C as i64 { c.insert(key(i), i); }
            c.run_pending_tasks();
            let r = bench(&mut || { let mut h = 0i64; for k in black_box(&hit_q).iter() { if c.get(k).is_some() { h += 1; } } h }, |&h| h == Q as i64);
            row("cache-hit", lib, ver, Q as u64, r, "");
        }
        if selected("cache-miss") {
            let r = bench(&mut || {
                let c: moka::sync::Cache<i64, i64> = moka::sync::Cache::new(C as u64);
                let mut m = 0i64;
                for (i, k) in black_box(&keys).iter().enumerate() { if c.get(k).is_none() { m += 1; c.insert(*k, i as i64); } }
                c.run_pending_tasks();
                (m, c.entry_count())
            }, |&r| r == (Q as i64, C as u64));
            row("cache-miss", lib, ver, Q as u64, r, "TinyLFU admission may reject a miss's insert; run_pending_tasks before the size");
        }
        if selected("cache-evict") {
            let r = bench(&mut || {
                let c: moka::sync::Cache<i64, i64> = moka::sync::Cache::new(C as u64);
                for (i, k) in black_box(&keys).iter().enumerate() { c.insert(*k, i as i64); }
                c.run_pending_tasks();
                c.entry_count()
            }, |&n| n == C as u64);
            row("cache-evict", lib, ver, Q as u64, r, "TinyLFU admission; run_pending_tasks before the size");
        }
        if selected("cache-ttl") {
            let r = bench(&mut || {
                let c: moka::sync::Cache<i64, i64> = moka::sync::Cache::builder()
                    .max_capacity(1_000_000).time_to_live(Duration::from_millis(1)).build();
                let ks = black_box(&keys[..1_000_000]);
                for (i, k) in ks.iter().enumerate() { c.insert(*k, i as i64); }
                std::thread::sleep(Duration::from_millis(2));
                let mut e = 0i64;
                for k in ks.iter() { if c.get(k).is_none() { e += 1; } }
                e
            }, |&e| e == 1_000_000);
            row("cache-ttl", lib, ver, 1_000_000, r, "time_to_live 1 ms; includes the 2 ms sleep");
        }
    }

    // ---- lru ----
    {
        let lib = "lru::LruCache";
        let ver = "0.18.2";
        let cap = NonZeroUsize::new(C).unwrap();
        if selected("cache-hit") {
            let mut c = lru::LruCache::new(cap);
            for i in 0..C as i64 { c.put(key(i), i); }
            let r = bench(&mut || { let mut h = 0i64; for k in black_box(&hit_q).iter() { if c.get(k).is_some() { h += 1; } } h }, |&h| h == Q as i64);
            row("cache-hit", lib, ver, Q as u64, r, "");
        }
        if selected("cache-miss") {
            let r = bench(&mut || {
                let mut c = lru::LruCache::new(cap);
                let mut m = 0i64;
                for (i, k) in black_box(&keys).iter().enumerate() { if c.get(k).is_none() { m += 1; c.put(*k, i as i64); } }
                (m, c.len())
            }, |&r| r == (Q as i64, C));
            row("cache-miss", lib, ver, Q as u64, r, "");
        }
        if selected("cache-evict") {
            let r = bench(&mut || {
                let mut c = lru::LruCache::new(cap);
                for (i, k) in black_box(&keys).iter().enumerate() { c.put(*k, i as i64); }
                c.len()
            }, |&n| n == C);
            row("cache-evict", lib, ver, Q as u64, r, "");
        }
        if selected("cache-ttl") {
            skip("cache-ttl", lib, 1_000_000, "lru::LruCache has no TTL (moka is the Rust TTL row)");
        }
    }
}
