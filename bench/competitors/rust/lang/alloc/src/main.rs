// Rust competitor: unit-5 lang/alloc benchmarks (contract: u5-competitor-spec.md).
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
// black_box on each new Box keeps the alloc/free pair from being elided (LLVM may
// remove unused heap allocations) and forces the four field stores to happen.

struct Obj { a: i64, b: i64, c: i64, d: i64 }
impl Obj { #[inline] fn new(v: i64) -> Obj { Obj { a: v, b: v, c: v, d: v } } }

// the Box is moved through depth non-inlined frames; the last reads .a and frees it.
// black_box on the result keeps each frame a real call (no tail-call loop).
#[inline(never)]
fn pass(o: Box<Obj>, depth: i32) -> i64 {
    if depth == 0 { return o.a; } // o freed on return
    let r = pass(o, depth - 1);
    black_box(r)
}

fn churn<const R: usize>(n: i64) -> i64 {
    let mut ring: Vec<Option<Box<Obj>>> = (0..R).map(|_| None).collect();
    let mut s: i64 = 0;
    for i in 0..n {
        let slot = &mut ring[(i as usize) % R];
        *slot = Some(black_box(Box::new(Obj::new(i)))); // previous occupant (oldest) dropped
        s += slot.as_ref().unwrap().a;
    }
    let ringsum: i64 = ring.iter().map(|p| p.as_ref().map_or(0, |o| o.a)).sum();
    black_box(ringsum);
    s
}

fn main() {
    let ctx = Ctx::new();
    const N: i64 = 10_000_000;

    ctx.run("alloc-small-object", N, "Box::new", "std", "", "", || {
        let mut s: i64 = 0;
        for i in 0..N {
            let o = black_box(Box::new(Obj::new(i)));
            s += o.a;
        } // o dropped (freed) at the end of each iteration
        s
    }, |s| s == 49999995000000);

    ctx.run("alloc-churn-1", N, "Vec<Option<Box>> ring", "std", "", "", || churn::<1>(N),
        |s| s == 49999995000000);
    ctx.run("alloc-churn-10", N, "Vec<Option<Box>> ring", "std", "", "", || churn::<10>(N),
        |s| s == 49999995000000);

    ctx.run("alloc-transfer-chain", 1_000_000, "Box move", "std", "", "", || {
        let mut s: i64 = 0;
        let depth = black_box(10);
        for i in 0..1_000_000 { s += pass(black_box(Box::new(Obj::new(i))), depth); }
        s
    }, |s| s == 499999500000);

    ctx.skipped("alloc-live-set-on", 0, "", "", "a Cajeta compiler mode (--live-set); no competitor counterpart", "");
    ctx.skipped("alloc-live-set-off", 0, "", "", "a Cajeta compiler mode (--live-set); no competitor counterpart", "");

    ctx.run("alloc-array", 100_000, "vec![0i32; 1024]", "std", "", "", || {
        let mut s: i64 = 0;
        for i in 0..100_000i64 {
            let mut a: Vec<i32> = black_box(vec![0i32; 1024]); // zeroed (alloc_zeroed)
            a[0] = i as i32; a[1023] = 1;
            let a = black_box(a);
            s += (a[0] + a[1023]) as i64;
        }
        s
    }, |s| s == 5000050000);

    ctx.run("alloc-struct-value", 100_000_000, "struct", "std", "", "", || {
        struct P { x: i64, y: i64 }
        let mask: i64 = black_box(-1);
        let mut s: i64 = 0;
        for i in 0..100_000_000i64 { let p = P { x: i, y: i & mask }; s += p.x + p.y; }
        s
    }, |s| s == 9999999900000000);
}
