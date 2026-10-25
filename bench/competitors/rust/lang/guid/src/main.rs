// Rust competitor: unit-5 lang/guid benchmarks (contract: u5-competitor-spec.md).
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
use std::cell::Cell;
use uuid::Uuid;

const UUID_VER: &str = "1.25.0";

fn distinct(ids: &[Uuid]) -> i64 {
    let mut v: Vec<u128> = ids.iter().map(|u| u.as_u128()).collect();
    v.sort_unstable();
    v.dedup();
    v.len() as i64
}

// vec![zero; n] is calloc-backed (lazily mapped pages); touch every page untimed so
// first-touch page faults are not charged to the first timed trial.
fn touched<T: Copy>(n: usize, z: T) -> Vec<T> {
    let mut v = vec![z; n];
    for x in v.iter_mut() { *x = black_box(z); }
    v
}

fn main() {
    let ctx = Ctx::new();
    const N: usize = 1_000_000;

    {
        // preallocated array of UUID values; the workload fills it and returns it,
        // the untimed check counts distinct values, clears it and hands it back.
        let slot: Cell<Vec<Uuid>> = Cell::new(touched(if ctx.selected("guid-generate") { N } else { 0 }, Uuid::nil()));
        ctx.run("guid-generate", N as i64, "uuid", UUID_VER,
            "Uuid::new_v4 into a preallocated Vec<Uuid>; default rng feature (getrandom)", "", || {
            let mut ids = slot.take();
            for k in 0..ids.len() { ids[k] = Uuid::new_v4(); }
            ids
        }, |mut ids: Vec<Uuid>| {
            let ok = distinct(&ids) == N as i64;
            for x in ids.iter_mut() { *x = Uuid::nil(); }
            slot.set(ids);
            ok
        });
    }

    if ctx.selected("guid-parse") || ctx.selected("guid-format") {
        let ids: Vec<Uuid> = (0..N).map(|_| Uuid::new_v4()).collect();
        let strs: Vec<String> = ids.iter().map(|u| u.hyphenated().to_string()).collect();

        let pslot: Cell<Vec<Uuid>> = Cell::new(touched(N, Uuid::nil()));
        ctx.run("guid-parse", N as i64, "uuid", UUID_VER, "Uuid::parse_str", "", || {
            let mut parsed = pslot.take();
            let strs: &Vec<String> = black_box(&strs);
            for k in 0..N { parsed[k] = Uuid::parse_str(&strs[k]).unwrap_or(Uuid::nil()); }
            parsed
        }, |mut parsed: Vec<Uuid>| {
            let eq = (0..N).filter(|&k| parsed[k] == ids[k]).count();
            for x in parsed.iter_mut() { *x = Uuid::nil(); } // the next trial must parse again
            pslot.set(parsed);
            eq == N
        });

        let oslot: Cell<Vec<[u8; 36]>> = Cell::new(touched(N, [0u8; 36]));
        ctx.run("guid-format", N as i64, "uuid", UUID_VER,
            "hyphenated().encode_lower into a preallocated [u8; 36] per uuid; under target-cpu=native (znver2) LLVM codegen of this encoder measured ~12x slower than the generic x86-64 build (113 ms vs 9.6 ms per 1e6 in a probe)", "", || {
            let mut out = oslot.take();
            let ids: &Vec<Uuid> = black_box(&ids);
            for k in 0..N { ids[k].hyphenated().encode_lower(&mut out[k]); }
            out
        }, |mut out: Vec<[u8; 36]>| {
            let mut total: i64 = 0;
            for k in 0..N {
                let len = out[k].iter().position(|&b| b == 0).unwrap_or(36);
                if &out[k][..len] == strs[k].as_bytes() { total += len as i64; }
            }
            for s in out.iter_mut() { *s = [0u8; 36]; }
            oslot.set(out);
            total == 36000000
        });
    }
}
