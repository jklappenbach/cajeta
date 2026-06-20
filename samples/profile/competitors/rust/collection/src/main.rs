// Rust collection competitors — the same four workloads the Cajeta side runs:
// hashmap-int (HashMap<int,int> put i→2i then sum lookups, N=50K), hashmap-string
// (HashMap<String,int> with "key"+i, N=30K), hashset-dedup (insert i%50000 over
// 100K → 50K distinct), arraylist-append (push 0..100K). One schema-conformant
// CSV row per (benchmark, library). Cross-checks are the exact closed forms the
// Cajeta benchmarks use. Libraries: std (SipHash) and ahash (fast hasher).
use ahash::AHashMap;
use std::collections::{HashMap, HashSet};
use std::time::Instant;

const N_INT: usize = 50_000;
const N_STR: usize = 30_000;
const N_SET: usize = 100_000;
const SET_HALF: usize = 50_000;
const N_VEC: usize = 100_000;
const INT_REF: i64 = 2_499_950_000; // N_INT*(N_INT-1)
const STR_REF: i64 = 449_985_000; // N_STR*(N_STR-1)/2

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

fn emit(run_id: &str, ts: &str, bench: &str, lib: &str, input: usize,
        warmup: usize, trials: usize, mut s: Vec<u128>, check_ok: bool) {
    s.sort_unstable();
    let n = s.len();
    let sum: u128 = s.iter().sum();
    let (mn, med, mean, p95) = (s[0], s[n / 2], sum / n as u128, s[(n * 95 / 100).min(n - 1)]);
    let mops = if med > 0 { input as f64 / med as f64 * 1e9 / 1e6 } else { 0.0 };
    let status = if check_ok { "ok" } else { "invalid" };
    println!(
        "1,{run_id},{ts},{bench},collection,,{input},,{input},rust,{ver},{lib},std,-O3 lto,{warmup},{trials},\
{mn},{med},{mean},{p95},{mops:.2},Mop/s,{rss},-1,-1,-1,-1,{status},{check},,",
        run_id = run_id, ts = ts, bench = bench, input = input, ver = env("PROFILE_LANG_VERSION", ""),
        lib = lib, warmup = warmup, trials = trials, mn = mn, med = med, mean = mean, p95 = p95,
        mops = mops, rss = peak_rss_kb(), status = status, check = check_ok);
}

fn bench<T, F: FnMut() -> T, C: Fn(&T) -> bool>(warmup: usize, trials: usize, mut f: F, check: C)
    -> (Vec<u128>, bool) {
    for _ in 0..warmup { let r = f(); std::hint::black_box(&r); }
    let mut s = Vec::with_capacity(trials);
    let mut ok = true;
    for _ in 0..trials {
        let t = Instant::now();
        let r = f();
        s.push(t.elapsed().as_nanos());
        ok = check(&r);
        std::hint::black_box(&r);
    }
    (s, ok)
}

fn main() {
    let run_id = env("PROFILE_RUN_ID", "local");
    let ts = env("PROFILE_RUN_TS", "");
    let warmup: usize = env("PROFILE_WARMUP", "3").parse().unwrap_or(3);
    let trials: usize = env("PROFILE_TRIALS", "10").parse().unwrap_or(10);

    // hashmap-int: std (SipHash)
    let (s, ok) = bench(warmup, trials, || {
        let mut m: HashMap<i64, i64> = HashMap::with_capacity(N_INT);
        for i in 0..N_INT as i64 { m.insert(i, i * 2); }
        let mut sum = 0i64;
        for j in 0..N_INT as i64 { sum += m[&j]; }
        sum
    }, |&r| r == INT_REF);
    emit(&run_id, &ts, "hashmap-int", "std-HashMap", N_INT, warmup, trials, s, ok);

    // hashmap-int: ahash
    let (s, ok) = bench(warmup, trials, || {
        let mut m: AHashMap<i64, i64> = AHashMap::with_capacity(N_INT);
        for i in 0..N_INT as i64 { m.insert(i, i * 2); }
        let mut sum = 0i64;
        for j in 0..N_INT as i64 { sum += m[&j]; }
        sum
    }, |&r| r == INT_REF);
    emit(&run_id, &ts, "hashmap-int", "ahash", N_INT, warmup, trials, s, ok);

    // hashmap-string: std
    let (s, ok) = bench(warmup, trials, || {
        let mut m: HashMap<String, i64> = HashMap::with_capacity(N_STR);
        for i in 0..N_STR as i64 { m.insert(format!("key{}", i), i); }
        let mut sum = 0i64;
        for j in 0..N_STR as i64 { sum += m[&format!("key{}", j)]; }
        sum
    }, |&r| r == STR_REF);
    emit(&run_id, &ts, "hashmap-string", "std-HashMap", N_STR, warmup, trials, s, ok);

    // hashmap-string: ahash
    let (s, ok) = bench(warmup, trials, || {
        let mut m: AHashMap<String, i64> = AHashMap::with_capacity(N_STR);
        for i in 0..N_STR as i64 { m.insert(format!("key{}", i), i); }
        let mut sum = 0i64;
        for j in 0..N_STR as i64 { sum += m[&format!("key{}", j)]; }
        sum
    }, |&r| r == STR_REF);
    emit(&run_id, &ts, "hashmap-string", "ahash", N_STR, warmup, trials, s, ok);

    // hashset-dedup: insert i%half over N ; distinct == half
    let (s, ok) = bench(warmup, trials, || {
        let mut set: HashSet<i64> = HashSet::new();
        for i in 0..N_SET as i64 { set.insert(i % SET_HALF as i64); }
        set.len()
    }, |&r| r == SET_HALF);
    emit(&run_id, &ts, "hashset-dedup", "std-HashSet", N_SET, warmup, trials, s, ok);

    // arraylist-append: push 0..N
    let (s, ok) = bench(warmup, trials, || {
        let mut v: Vec<i64> = Vec::new();
        for i in 0..N_VEC as i64 { v.push(i); }
        v.len()
    }, |&r| r == N_VEC);
    emit(&run_id, &ts, "arraylist-append", "Vec", N_VEC, warmup, trials, s, ok);
}
