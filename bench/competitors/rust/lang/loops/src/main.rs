// Rust competitor: unit-5 lang/loops benchmarks (contract: u5-competitor-spec.md).
// ---- common harness (identical in every unit-5 rust lang program) ----
// One v1-shape CSV row (32 columns) per (benchmark, variant, library). Each
// workload returns its result; the check is applied to every timed trial's
// result (and to the warmups), so nothing can be dead-code eliminated. OPAQUE
// values go through std::hint::black_box.
#![allow(dead_code)]
use std::hint::black_box;
use std::time::Instant;

const FLAGS: &str = "-C target-cpu=native -C opt-level=3 lto";

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }

fn field(s: &str) -> String { s.replace(',', ";").replace('\n', " ") }

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

struct Ctx {
    run_id: String, ts: String, ver: String,
    warmup: usize, trials: usize, only: Vec<String>,
}

impl Ctx {
    fn new() -> Ctx {
        let warmup = env("PROFILE_WARMUP", "3").trim().parse::<i64>().unwrap_or(3).max(0) as usize;
        let trials = env("PROFILE_TRIALS", "10").trim().parse::<i64>().unwrap_or(10).max(1) as usize;
        let only = env("LANG_ONLY", "").split(',').map(|t| t.trim().to_string()).filter(|t| !t.is_empty()).collect();
        Ctx { run_id: field(&env("PROFILE_RUN_ID", "local")), ts: field(&env("PROFILE_RUN_TS", "")),
              ver: field(&env("PROFILE_LANG_VERSION", "")), warmup, trials, only }
    }

    fn selected(&self, slug: &str) -> bool { self.only.is_empty() || self.only.iter().any(|t| t == slug) }

    fn emit(&self, slug: &str, input: i64, lib: &str, libver: &str, mut s: Vec<i64>, ok: bool, notes: &str, variant: &str) {
        s.sort_unstable();
        let n = s.len();
        let sum: i64 = s.iter().sum();
        let (mn, med, mean, p95) = (s[0], s[n / 2], sum / n as i64, s[(n * 95 / 100).min(n - 1)]);
        let mops = if med > 0 { input as f64 / med as f64 * 1e9 / 1e6 } else { 0.0 };
        println!("1,{},{},{},lang,,{},,{},rust,{},{},{},{},{},{},{},{},{},{},{:.2},Mop/s,{},-1,-1,-1,-1,{},{},{},{},cpu",
            self.run_id, self.ts, slug, input, input, self.ver, field(lib), field(libver), FLAGS,
            self.warmup, self.trials, mn, med, mean, p95, mops, peak_rss_kb(),
            if ok { "ok" } else { "invalid" }, if ok { "true" } else { "false" }, field(notes), field(variant));
    }

    fn skipped(&self, slug: &str, input: i64, lib: &str, libver: &str, reason: &str, variant: &str) {
        if !self.selected(slug) { return; }
        println!("1,{},{},{},lang,,{},,{},rust,{},{},{},{},{},{},-1,-1,-1,-1,,Mop/s,{},-1,-1,-1,-1,skipped,,{},{},cpu",
            self.run_id, self.ts, slug, input, input, self.ver, field(lib), field(libver), FLAGS,
            self.warmup, self.trials, peak_rss_kb(), field(reason), field(variant));
    }

    // warmup untimed runs, then timed trials (monotonic clock); check every result.
    fn run<T, F: FnMut() -> T, C: FnMut(T) -> bool>(&self, slug: &str, input: i64, lib: &str, libver: &str,
            notes: &str, variant: &str, mut f: F, mut check: C) {
        if !self.selected(slug) { return; }
        for _ in 0..self.warmup { let r = f(); black_box(check(r)); }
        let mut s = Vec::with_capacity(self.trials);
        let mut ok = true;
        for _ in 0..self.trials {
            let t0 = Instant::now();
            let r = f();
            let dt = t0.elapsed().as_nanos() as i64;
            s.push(dt);
            if !check(black_box(r)) { ok = false; }
        }
        self.emit(slug, input, lib, libver, s, ok, notes, variant);
    }
}
// ---- end common harness ----

fn main() {
    let ctx = Ctx::new();
    const N: i64 = 100_000_000;

    ctx.run("loop-counted", N, "while", "std", "", "", || {
        let mask: i64 = black_box(-1);
        let mut s: i64 = 0;
        let mut i: i64 = 0;
        while i < N { s += i & mask; i += 1; }
        s
    }, |s| s == 4999999950000000);

    {
        let need = ctx.selected("loop-counted-nobounds");
        let a: Vec<i32> = (0..if need { 1_000_000 } else { 0 }).collect();
        ctx.run("loop-counted-nobounds", 100 * 1_000_000, "slice index", "std",
            "a[k] is checked; LLVM may drop checks it proves redundant", "bounds-on", || {
            let a: &[i32] = black_box(&a[..]);
            let n = a.len();
            let mut s: i64 = 0;
            for _ in 0..100 { for k in 0..n { s += a[k] as i64; } }
            s
        }, |s| s == 49999950000000);
        ctx.run("loop-counted-nobounds", 100 * 1_000_000, "get_unchecked", "std", "", "bounds-off", || {
            let a: &[i32] = black_box(&a[..]);
            let n = a.len();
            let mut s: i64 = 0;
            for _ in 0..100 { for k in 0..n { s += unsafe { *a.get_unchecked(k) } as i64; } }
            s
        }, |s| s == 49999950000000);
    }

    ctx.run("loop-nested", 100_000_000, "for", "std", "", "", || {
        let n = black_box(10_000i64);
        let mut s: i64 = 0;
        for i in 0..n { for j in 0..n { s += i ^ j; } }
        s
    }, |s| s == 642122061696);

    if ctx.selected("loop-foreach-array") || ctx.selected("loop-foreach-arraylist") {
        let mut a: Vec<i32> = vec![0; 10_000_000];
        for k in 0..10_000_000usize { a[k] = (k % 1000) as i32; }
        let a: Box<[i32]> = a.into_boxed_slice();
        ctx.run("loop-foreach-array", 10_000_000, "slice iter", "std", "", "", || {
            let v: &[i32] = black_box(&a[..]);
            let mut s: i64 = 0;
            for &x in v.iter() { s += x as i64; }
            s
        }, |s| s == 4995000000);
        let mut l: Vec<i32> = Vec::new();
        for k in 0..10_000_000i32 { l.push(k % 1000); }
        ctx.run("loop-foreach-arraylist", 10_000_000, "Vec push iter", "std",
            "Vec built by push; iterated as a slice like loop-foreach-array", "", || {
            let v: &Vec<i32> = black_box(&l);
            let mut s: i64 = 0;
            for &x in v.iter() { s += x as i64; }
            s
        }, |s| s == 4995000000);
    }

    ctx.run("loop-range", N, "Range", "std", "", "", || {
        let mask: i64 = black_box(-1);
        let mut s: i64 = 0;
        for i in 0..N { s += i & mask; }
        s
    }, |s| s == 4999999950000000);

    if ctx.selected("loop-reduce") {
        let x: Vec<f64> = (0..10_000_000usize).map(|k| (k % 1000) as f64 * 0.5).collect();
        ctx.run("loop-reduce", 10_000_000, "for", "std", "sequential; no fast-math", "", || {
            let x: &[f64] = black_box(&x[..]);
            let mut s = 0.0f64;
            for k in 0..x.len() { s += x[k]; }
            s
        }, |s| (s - 2497500000.0).abs() <= 1e-6 * 2497500000.0);
    }

    if ctx.selected("loop-early-exit") {
        let mut a: Vec<i32> = (0..10_000_000).collect();
        a[5_000_000] = -1;
        ctx.run("loop-early-exit", 1000 * 10_000_000, "for+break", "std", "", "", || {
            let mut s: i64 = 0;
            for _ in 0..1000 {
                let p: &[i32] = black_box(&a[..]); // re-scan each repetition; no hoisting of the search
                let mut found: i64 = -1;
                for k in 0..p.len() { if p[k] < 0 { found = k as i64; break; } }
                s += found;
            }
            s
        }, |s| s == 5000000000);
    }
}
