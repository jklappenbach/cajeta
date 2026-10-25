// The wire/ competitor crate's row helper (copied from codec/basen, area wire,
// throughput unit a parameter): the v1 row
// (competitors/columns.txt first 31 columns; lib/rows.sh lifts it to v2),
// trial statistics, peak RSS and a counting allocator for alloc_bytes.
#![allow(dead_code)]
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

static ALLOCED: AtomicU64 = AtomicU64::new(0);
pub struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        ALLOCED.fetch_add(l.size() as u64, Ordering::Relaxed);
        System.alloc(l)
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { System.dealloc(p, l) }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        ALLOCED.fetch_add(n as u64, Ordering::Relaxed);
        System.realloc(p, l, n)
    }
}

/// Bytes requested from the allocator by one untimed execution of `f`.
pub fn alloc_of<F: FnMut()>(mut f: F) -> i64 {
    let a = ALLOCED.load(Ordering::Relaxed);
    f();
    (ALLOCED.load(Ordering::Relaxed) - a) as i64
}

pub fn env(k: &str, d: &str) -> String {
    std::env::var(k).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| d.to_string())
}
pub fn env_n(k: &str, d: usize) -> usize { env(k, "").parse().unwrap_or(d) }
pub fn smoke() -> bool { matches!(env("CAJETA_BENCH_SMOKE", "").as_str(), "1" | "true" | "yes") }

pub fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok()))
        .unwrap_or(-1)
}

pub struct Stats { pub min: u128, pub median: u128, pub mean: u128, pub p95: u128 }

/// `warmup` untimed runs, then `trials` timed runs of `f`.
pub fn measure<F: FnMut()>(warmup: usize, trials: usize, mut f: F) -> Stats {
    for _ in 0..warmup { f(); }
    let mut v = Vec::with_capacity(trials);
    for _ in 0..trials.max(1) {
        let t = Instant::now();
        f();
        v.push(t.elapsed().as_nanos());
    }
    v.sort_unstable();
    let n = v.len();
    Stats { min: v[0], median: v[n / 2], mean: v.iter().sum::<u128>() / n as u128,
            p95: v[(n * 95 / 100).min(n - 1)] }
}

pub const FLAGS: &str = "-O3 lto target-cpu=native";

/// unit "MB/s": input is bytes; "records/s": input is records.
pub fn emit(slug: &str, input: i64, lib: &str, ver: &str, warmup: usize, trials: usize, s: &Stats,
            alloc: i64, ok: bool, notes: &str, variant: &str, unit: &str) {
    let thr = if s.median > 0 {
        let mut per = input as f64 / s.median as f64 * 1e9;
        if unit == "MB/s" { per /= 1048576.0; }
        format!("{:.1}", per)
    } else { String::new() };
    println!("1,{},{},{},wire,,-1,,{},rust,{},{},{},{},{},{},{},{},{},{},{},{},{},-1,{},-1,-1,{},{},{},{}",
        env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, input,
        env("PROFILE_LANG_VERSION", ""), lib, ver, FLAGS, warmup, trials,
        s.min, s.median, s.mean, s.p95, thr, unit, peak_rss_kb(), alloc,
        if ok { "ok" } else { "invalid" }, ok, notes.replace(',', ";"), variant);
}
