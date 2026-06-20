// Rust stream competitors — the same two workloads the Cajeta side runs over
// xs[i] = i % 1000 (N=1M): stream-filter-map-reduce (filter evens → map +1 → sum,
// sequential iterator chain) and stream-parallel-reduce (sum via rayon par_iter).
// One schema-conformant CSV row per benchmark. Cross-check = the exact reference
// sums (250000000 / 499500000). Libraries: std Iterator and rayon.
use rayon::prelude::*;
use std::time::Instant;

const N: usize = 1_000_000;
const FMR_REF: i64 = 250_000_000;
const PR_REF: i64 = 499_500_000;

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
        "1,{run_id},{ts},{bench},stream,,{input},,{input},rust,{ver},{lib},std,-O3 lto,{warmup},{trials},\
{mn},{med},{mean},{p95},{mops:.2},Mop/s,{rss},-1,-1,-1,-1,{status},{check},,",
        run_id = run_id, ts = ts, bench = bench, input = input, ver = env("PROFILE_LANG_VERSION", ""),
        lib = lib, warmup = warmup, trials = trials, mn = mn, med = med, mean = mean, p95 = p95,
        mops = mops, rss = peak_rss_kb(), status = status, check = check_ok);
}

fn bench<F: FnMut() -> i64, C: Fn(i64) -> bool>(warmup: usize, trials: usize, mut f: F, check: C)
    -> (Vec<u128>, bool) {
    for _ in 0..warmup { std::hint::black_box(f()); }
    let mut s = Vec::with_capacity(trials);
    let mut ok = true;
    for _ in 0..trials {
        let t = Instant::now();
        let r = f();
        s.push(t.elapsed().as_nanos());
        ok = check(r);
        std::hint::black_box(r);
    }
    (s, ok)
}

fn main() {
    let run_id = env("PROFILE_RUN_ID", "local");
    let ts = env("PROFILE_RUN_TS", "");
    let warmup: usize = env("PROFILE_WARMUP", "3").parse().unwrap_or(3);
    let trials: usize = env("PROFILE_TRIALS", "10").parse().unwrap_or(10);
    let xs: Vec<i64> = (0..N as i64).map(|i| i % 1000).collect();

    // filter-map-reduce (sequential iterator chain)
    let (s, ok) = bench(warmup, trials, || {
        xs.iter().filter(|&&x| x % 2 == 0).map(|&x| x + 1).sum::<i64>()
    }, |r| r == FMR_REF);
    emit(&run_id, &ts, "stream-filter-map-reduce", "Iterator", N, warmup, trials, s, ok);

    // parallel-reduce (rayon)
    let (s, ok) = bench(warmup, trials, || {
        xs.par_iter().sum::<i64>()
    }, |r| r == PR_REF);
    emit(&run_id, &ts, "stream-parallel-reduce", "rayon", N, warmup, trials, s, ok);
}
