// Rust competitor: unit-5 lang/dispatch benchmarks (contract: u5-competitor-spec.md).
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
use std::any::Any;
use std::ops::Add;

#[inline(never)]
fn direct(x: i64) -> i64 { x + 1 }
fn inc(x: i64) -> i64 { x + 1 }

// call-virtual: no inheritance; a trait with an overridable default method, two overriding impls
trait VBase { fn f(&self, x: i64) -> i64 { x } }
struct VA; struct VB;
impl VBase for VA { fn f(&self, x: i64) -> i64 { x + 1 } }
impl VBase for VB { fn f(&self, x: i64) -> i64 { x + 2 } }

// call-interface: a trait with a required method, two implementations
trait IFace { fn f(&self, x: i64) -> i64; }
struct IA; struct IB;
impl IFace for IA { fn f(&self, x: i64) -> i64 { x + 1 } }
impl IFace for IB { fn f(&self, x: i64) -> i64 { x + 2 } }

fn combine<T: Add<Output = T>>(a: T, b: T) -> T { a + b }

struct Base { _t: u8 }
struct Mid { _t: u8 }
struct Leaf { _t: u8 }

fn main() {
    let ctx = Ctx::new();
    const N: i64 = 100_000_000;

    ctx.run("call-direct", N, "inline(never) fn", "std", "", "", || {
        let mask: i64 = black_box(-1);
        let mut s: i64 = 0;
        for i in 0..N { s += direct(i & mask); }
        s
    }, |s| s == 5000000050000000);

    {
        // the array's dynamic types are hidden so the call is a real vtable dispatch
        let objs: [Box<dyn VBase>; 2] = black_box([Box::new(VA), Box::new(VB)]);
        ctx.run("call-virtual", N, "Box<dyn Trait>", "std", "no inheritance: trait object", "", || {
            let objs = black_box(&objs);
            let mut s: i64 = 0;
            for i in 0..N { s += objs[(i & 1) as usize].f(i); }
            s
        }, |s| s == 5000000100000000);
    }
    {
        let objs: [Box<dyn IFace>; 2] = black_box([Box::new(IA), Box::new(IB)]);
        ctx.run("call-interface", N, "dyn Trait", "std", "", "", || {
            let objs = black_box(&objs);
            let mut s: i64 = 0;
            for i in 0..N { s += objs[(i & 1) as usize].f(i); }
            s
        }, |s| s == 5000000100000000);
    }

    ctx.run("call-template", N, "generic fn T: Add", "std", "monomorphized and inlined", "", || {
        let mut s: i64 = 0;
        for i in 0..N { s += combine::<i32>((i & 1023) as i32, 1) as i64; }
        s
    }, |s| s == 51249901696);

    ctx.run("call-lambda", N, "closure", "std",
        "capturing closure in a let binding; inlined; an empty asm on the sum each iteration stops LLVM folding the loop to its closed form", "", || {
        let k: i64 = black_box(1);
        let f = |x: i64| x + k;
        let mut s: i64 = 0;
        for i in 0..N {
            s += f(i);
            unsafe { std::arch::asm!("/* {0} */", inout(reg) s, options(nomem, nostack, preserves_flags)); }
        }
        s
    }, |s| s == 5000000050000000);

    {
        // hide the pointer so the call is a real indirect call
        let g0: fn(i64) -> i64 = black_box(inc as fn(i64) -> i64);
        ctx.run("call-method-ref", N, "fn pointer", "std", "", "", || {
            let g: fn(i64) -> i64 = black_box(g0);
            let mut s: i64 = 0;
            for i in 0..N { s += g(i); }
            s
        }, |s| s == 5000000050000000);
    }

    {
        let (b, m, l) = (Base { _t: 0 }, Mid { _t: 1 }, Leaf { _t: 2 });
        let objs: [&dyn Any; 3] = black_box([&b, &m, &l]);
        ctx.run("instanceof", N, "dyn Any is", "std", "no inheritance: Any over concrete types; is::<Mid>() || is::<Leaf>()", "", || {
            let objs = black_box(&objs);
            let mut count: i64 = 0;
            for i in 0..N {
                let o = objs[(i % 3) as usize];
                if o.is::<Mid>() || o.is::<Leaf>() { count += 1; }
            }
            count
        }, |c| c == 66666666);
    }
}
