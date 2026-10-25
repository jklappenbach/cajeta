// Rust competitor: unit-5 lang/exceptions benchmarks (contract: u5-competitor-spec.md).
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
// Rust has no exceptions: every "throw" is an Err returned and propagated with ? through the same depth.

#[derive(Debug)]
struct BenchError { msg: &'static str }

#[inline(never)]
fn f_maybe_throw(i: i64, mask: i64) -> Result<i64, BenchError> {
    if i < 0 { return Err(BenchError { msg: "bench error" }); }
    Ok(i & mask)
}

// depth frames of non-inlined recursion; the innermost "throws" when i >= 0 (always).
// The Err is propagated with ? through every frame; black_box on the Ok value keeps
// each frame a real call (no tail call / loop conversion).
#[inline(never)]
fn thrower(depth: i32, i: i64) -> Result<i64, BenchError> {
    if depth <= 1 {
        if i >= 0 { return Err(BenchError { msg: "bench error" }); }
        return Ok(i);
    }
    let r = thrower(depth - 1, i)?;
    Ok(black_box(r) + 1)
}

fn throw_catch(depth: i32, iters: i64) -> i64 {
    let mut count: i64 = 0;
    let depth = black_box(depth);
    for i in 0..iters {
        match thrower(depth, black_box(i)) {
            Ok(r) => count -= r, // never reached
            Err(e) => { black_box(&e); count += 1; }
        }
    }
    count
}

const NOTE: &str = "Rust has no exceptions: Result propagated with ?";

fn main() {
    let ctx = Ctx::new();
    const N: i64 = 100_000_000;

    ctx.run("try-no-throw", N, "Result match", "std",
        "Rust has no exceptions: Result checked per call", "", || {
        let mask: i64 = black_box(-1);
        let mut s: i64 = 0;
        for i in 0..N {
            match f_maybe_throw(i, mask) { Ok(v) => s += v, Err(_) => s -= 1 }
        }
        s
    }, |s| s == 4999999950000000);

    ctx.run("throw-catch-1", 1_000_000, "Result ?", "std", NOTE, "", || throw_catch(1, 1_000_000),
        |c| c == 1_000_000);
    ctx.run("throw-catch-10", 1_000_000, "Result ?", "std", NOTE, "", || throw_catch(10, 1_000_000),
        |c| c == 1_000_000);
    ctx.run("throw-catch-100", 100_000, "Result ?", "std", NOTE, "", || throw_catch(100, 100_000),
        |c| c == 100_000);

    ctx.skipped("throw-recoverable", 1_000_000, "Result ?", "std",
        "no recoverable/unrecoverable distinction; the page's competitor is Java", "");
    ctx.skipped("throw-in-fiber", 100_000, "Result ?", "std",
        "Rust has no fiber without an async runtime crate; none is pinned", "");
}
