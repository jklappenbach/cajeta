// The collection/ competitors' shared inputs and row format (Rust side).
// Mirrors bench/src/main/cajeta/bench/collection/CollGen.cajeta exactly: the
// same draws in the same order give the same keys, queries and checks in all
// six languages.
//   next(): s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s >> 33
//           (xorshift64 on u64, seed 88172645463325252; a 31-bit draw)
//   key(i): three xorshift64 rounds of i ^ 0x5851F42D4C957F2D, as i64
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
#[global_allocator]
static GA: Counting = Counting;

pub fn xs(mut x: u64) -> u64 {
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    x
}

pub fn key(i: i64) -> i64 {
    xs(xs(xs((i as u64) ^ 0x5851F42D4C957F2D))) as i64
}

pub struct Gen { s: u64 }
impl Gen {
    pub fn new() -> Gen { Gen { s: 88172645463325252 } }
    pub fn next(&mut self) -> i32 { self.s = xs(self.s); (self.s >> 33) as i32 }
    pub fn indices(&mut self, count: usize, range: i32) -> Vec<i32> {
        (0..count).map(|_| self.next() % range).collect()
    }
}

pub fn env(k: &str, d: &str) -> String { std::env::var(k).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| d.to_string()) }
pub fn params() -> (usize, usize) {
    (env("PROFILE_WARMUP", "3").parse().unwrap_or(3), env("PROFILE_TRIALS", "10").parse().unwrap_or(10))
}
pub fn selected(id: &str) -> bool {
    let s = env("PROFILE_BENCH", "");
    s.is_empty() || s.split_whitespace().any(|x| x == id)
}

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:")).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

pub struct Measured { pub samples: Vec<u128>, pub ok: bool, pub alloc: u64 }

/// Warm up, time `trials` calls of f, check the last result (untimed), then
/// one more untimed call under the counting allocator for alloc_bytes.
pub fn bench<T, F: FnMut() -> T, C: Fn(&T) -> bool>(f: &mut F, check: C) -> Measured {
    let (warmup, trials) = params();
    for _ in 0..warmup { std::hint::black_box(f()); }
    let mut samples = Vec::with_capacity(trials);
    let mut ok = true;
    for _ in 0..trials {
        let t = Instant::now();
        let r = f();
        samples.push(t.elapsed().as_nanos());
        ok = check(&r);
        std::hint::black_box(&r);
    }
    let a0 = ALLOCED.load(Ordering::Relaxed);
    std::hint::black_box(f());
    let alloc = ALLOCED.load(Ordering::Relaxed) - a0;
    Measured { samples, ok, alloc }
}

pub fn row(id: &str, lib: &str, libver: &str, input: u64, m: Measured, notes: &str) {
    let (warmup, trials) = params();
    let mut s = m.samples;
    s.sort_unstable();
    let n = s.len();
    let sum: u128 = s.iter().sum();
    let (mn, med, mean, p95) = (s[0], s[n / 2], sum / n as u128, s[(n * 95 / 100).min(n - 1)]);
    let mops = if med > 0 { input as f64 / med as f64 * 1e3 } else { 0.0 };
    println!(
        "1,{},{},{id},collection,,{input},,{input},rust,{},{lib},{libver},-O3 lto target-cpu=native,{warmup},{trials},\
{mn},{med},{mean},{p95},{mops:.2},Mop/s,{},-1,{},-1,-1,{},{},{},",
        env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", ""),
        peak_rss_kb(), m.alloc, if m.ok { "ok" } else { "invalid" }, m.ok, notes.replace(',', " "));
}

pub fn skip(id: &str, lib: &str, input: u64, reason: &str) {
    println!("1,{},{},{id},collection,,{input},,{input},rust,{},{lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{},",
        env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", ""), reason.replace(',', " "));
}
