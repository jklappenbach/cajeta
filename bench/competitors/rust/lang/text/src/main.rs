// Rust competitor: unit-5 lang/text benchmarks (contract: u5-competitor-spec.md).
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
use std::fmt::Write as _;

fn main() {
    let ctx = Ctx::new();

    if ctx.selected("string-codepoint-walk") {
        let base = "Hello, wörld! Καλημέρα κόσμε, こんにちは世界 \u{1F680}✨ ";
        if base.len() != 74 { eprintln!("base size {} != 74", base.len()); std::process::exit(1); }
        let text: String = base.repeat(14170);
        ctx.run("string-codepoint-walk", text.len() as i64, "str::chars", "std", "", "", || {
            let t: &str = black_box(text.as_str());
            let (mut count, mut sum) = (0i64, 0i64);
            for c in t.chars() { count += 1; sum += c as i64; }
            (count, sum)
        }, |(c, s)| c == 580970 && s == 3748489290);
    }

    if ctx.selected("string-byteat-scan") {
        let text: String = "the quick brown fox jumps over the lazy dog\n".repeat(23832);
        ctx.run("string-byteat-scan", text.len() as i64, "str::bytes", "std", "", "", || {
            let t: &str = black_box(text.as_str());
            let mut c: i64 = 0;
            for b in t.bytes() { if b == b'\n' { c += 1; } }
            c
        }, |c| c == 23832);
    }

    if ctx.selected("string-split") {
        let mut text: String = "alpha,beta,gamma,delta,epsilon,zeta,eta,theta,iota,kappa,".repeat(18397);
        text.push_str("end");
        ctx.run("string-split", 100 * text.len() as i64, "str::split collect", "std",
            "split(',').collect::<Vec<&str>>()", "", || {
            let mut total: i64 = 0;
            for _ in 0..100 {
                let t: &str = black_box(text.as_str());
                let pieces: Vec<&str> = t.split(',').collect();
                total += black_box(&pieces).len() as i64;
            }
            total
        }, |t| t == 18397100);
    }

    if ctx.selected("string-compare") {
        let mut a: Vec<String> = Vec::with_capacity(64);
        let mut b: Vec<String> = Vec::with_capacity(64);
        for k in 0..64 {
            let mut s = String::from("abcdefghijklmnopqrstuvwxyz0123");
            s.push((b'0' + (k / 10) as u8) as char);
            s.push((b'0' + (k % 10) as u8) as char);
            let mut c = String::with_capacity(32);
            c.push_str(&s); // a fresh copy
            if k & 1 == 1 { c.pop(); c.push('x'); }
            a.push(s); b.push(c);
        }
        ctx.run("string-compare", 10_000_000, "String ==", "std", "", "", || {
            let (a, b) = black_box((&a, &b));
            let mut eq: i64 = 0;
            for i in 0..10_000_000usize { if a[i & 63] == b[i & 63] { eq += 1; } }
            eq
        }, |e| e == 5000000);
    }

    ctx.run("stringbuilder-append", 1_000_000, "String::push_str", "std",
        "String is its own builder: the final string is a move", "", || {
        let mut sb = String::new();
        let piece: &str = black_box("0123456789abcdef");
        for _ in 0..1_000_000 { sb.push_str(piece); }
        let out: String = sb;
        black_box(&out).len() as i64
    }, |n| n == 16000000);

    ctx.run("stringbuilder-append-mixed", 1_000_000, "String + write!", "std",
        "write! of i32 and f64 (Display is shortest round-trip)", "", || {
        let mut sb = String::new();
        for i in 0..1_000_000i32 {
            match i % 3 {
                0 => { write!(sb, "{}", i).unwrap(); }
                1 => { write!(sb, "{}", (i % 1000) as f64 + 0.5).unwrap(); }
                _ => sb.push_str("abc"),
            }
        }
        let out: String = sb;
        black_box(&out).len() as i64
    }, |n| n == 4592962);

    if ctx.selected("stringbuilder-tostring") {
        let builder: String = "0123456789abcdef".repeat(1048576);
        ctx.run("stringbuilder-tostring", 100, "String::clone", "std", "copies 16 MiB per materialization", "", || {
            let mut total: i64 = 0;
            for _ in 0..100 {
                let s: String = black_box(&builder).clone();
                total += black_box(&s).len() as i64; // the copy must really happen
            }
            total
        }, |t| t == 1677721600);
    }
}
