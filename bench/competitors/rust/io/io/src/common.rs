// Shared by io-std and io-tokio: v1 rows (31 columns, competitors/columns.txt
// order, area io; lib/rows.sh lifts them to v2), trial statistics, sizes.
#![allow(dead_code)]
use std::time::Instant;

pub fn env(k: &str, d: &str) -> String {
    std::env::var(k).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| d.to_string())
}
pub fn env_n(k: &str, d: usize) -> usize { env(k, "").parse().unwrap_or(d) }
pub fn smoke() -> bool { matches!(env("CAJETA_BENCH_SMOKE", "").as_str(), "1" | "true" | "yes") }
pub fn pick(full: u64, sm: u64) -> u64 { if smoke() { sm } else { full } }
pub fn warm(d: usize) -> usize { if smoke() { 1 } else { env_n("PROFILE_WARMUP", d) } }
pub fn trials(d: usize) -> usize { if smoke() { env_n("PROFILE_TRIALS", 3) } else { env_n("PROFILE_TRIALS", d) } }
pub fn sm(what: &str) -> String {
    if smoke() { format!(" (smoke; the page's {what} is the full run)") } else { String::new() }
}

fn status_kb(key: &str) -> i64 {
    std::fs::read_to_string("/proc/self/status").ok()
        .and_then(|s| s.lines().find(|l| l.starts_with(key))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok()))
        .unwrap_or(-1)
}
pub fn peak_rss_kb() -> i64 { status_kb("VmHWM:") }
pub fn rss_kb() -> i64 { status_kb("VmRSS:") }

/// utime + stime of this process in milliseconds (USER_HZ = 100).
pub fn cpu_ms() -> i64 {
    let s = std::fs::read_to_string("/proc/self/stat").unwrap_or_default();
    let rest = s.rsplit_once(')').map(|(_, r)| r).unwrap_or("");
    let f: Vec<&str> = rest.split_whitespace().collect();
    // rest starts at field 3; utime is field 14, stime 15
    let u: i64 = f.get(11).and_then(|v| v.parse().ok()).unwrap_or(0);
    let st: i64 = f.get(12).and_then(|v| v.parse().ok()).unwrap_or(0);
    (u + st) * 10
}

#[derive(Clone, Copy)]
pub struct Stats { pub min: u128, pub median: u128, pub mean: u128, pub p95: u128 }

pub fn stats_of(mut v: Vec<u128>) -> Stats {
    v.sort_unstable();
    let n = v.len();
    Stats { min: v[0], median: v[n / 2], mean: v.iter().sum::<u128>() / n as u128, p95: v[(n * 95 / 100).min(n - 1)] }
}

/// `warmup` untimed runs, then `trials` timed runs; the first Err aborts.
pub fn measure<E, F: FnMut() -> Result<(), E>>(warmup: usize, trials: usize, mut f: F) -> Result<Stats, E> {
    for _ in 0..warmup { f()?; }
    let mut v = Vec::with_capacity(trials);
    for _ in 0..trials.max(1) {
        let t = Instant::now();
        f()?;
        v.push(t.elapsed().as_nanos());
    }
    Ok(stats_of(v))
}

pub const FLAGS: &str = "-O3 lto target-cpu=native";

pub fn clean(s: &str) -> String { s.replace(',', ";").replace('\n', " ") }

/// input = the throughput numerator (MiB for MB/s, items otherwise).
pub fn emit(slug: &str, input: i64, lib: &str, ver: &str, warmup: usize, trials: usize, s: &Stats, ok: bool, notes: &str, unit: &str) {
    let (thr, u) = if !unit.is_empty() && s.median > 0 && input >= 0 {
        (format!("{:.3}", input as f64 * 1e9 / s.median as f64), unit)
    } else { (String::new(), "") };
    let nt = format!("{}{}", clean(notes), if smoke() { "; smoke" } else { "" });
    println!("1,{},{},{},io,,-1,,{},rust,{},{},{},{},{},{},{},{},{},{},{},{},{},-1,-1,-1,-1,{},{},{},",
        env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, input, env("PROFILE_LANG_VERSION", ""),
        lib, ver, FLAGS, warmup, trials, s.min, s.median, s.mean, s.p95, thr, u, peak_rss_kb(),
        if ok { "ok" } else { "invalid" }, ok, nt);
}

/// A row whose workload returned an error: status invalid, the error in notes.
pub fn emit_err(slug: &str, lib: &str, ver: &str, err: &dyn std::fmt::Display) {
    println!("1,{},{},{},io,,-1,,-1,rust,{},{},{},{},0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,invalid,false,{},",
        env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, env("PROFILE_LANG_VERSION", ""),
        lib, ver, FLAGS, clean(&format!("error: {err}")));
}

pub fn lock_version(name: &str) -> String {
    let lock = include_str!("../Cargo.lock");
    let mut it = lock.lines();
    while let Some(l) = it.next() {
        if l == format!("name = \"{name}\"") {
            if let Some(v) = it.next() {
                return v.trim_start_matches("version = ").trim_matches('"').to_string();
            }
        }
    }
    String::new()
}

pub fn want(only: &[String], id: &str) -> bool { only.is_empty() || only.iter().any(|o| o == id) }
