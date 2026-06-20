// Rust time competitors — the same two workloads the Cajeta side runs:
// time-instant-arith (1M: instant from epoch-second i, + 7s, read epoch-second,
// accumulate) and time-localdate-arith (100K: date from epoch-day i, + 1 day,
// read epoch-day, accumulate). One schema-conformant CSV row per benchmark.
// Cross-check = the exact closed-form sum (500006500000 / 5000050000). Library:
// chrono (DateTime<Utc> / NaiveDate) — the competitor to Cajeta's value types.
use chrono::{DateTime, Days, NaiveDate, TimeDelta};
use std::time::Instant;
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicU64, Ordering as _Ord};
static _ALLOCED: AtomicU64 = AtomicU64::new(0);
static _LAST: AtomicU64 = AtomicU64::new(0);
struct _Counting;
unsafe impl GlobalAlloc for _Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { _ALLOCED.fetch_add(l.size() as u64, _Ord::Relaxed); System.alloc(l) }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { System.dealloc(p, l) }
}
#[global_allocator]
static _GA: _Counting = _Counting;
fn _alloc_of<F: FnOnce()>(run: F) { _ALLOCED.store(0, _Ord::Relaxed); run(); _LAST.store(_ALLOCED.load(_Ord::Relaxed), _Ord::Relaxed); }
fn _la() -> u64 { _LAST.load(_Ord::Relaxed) }

const N_INSTANT: usize = 1_000_000;
const N_DATE: usize = 100_000;
const INSTANT_REF: i64 = 500_006_500_000;
const DATE_REF: i64 = 5_000_050_000;

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

fn emit(run_id: &str, ts: &str, bench: &str, input: usize,
        warmup: usize, trials: usize, mut s: Vec<u128>, check_ok: bool) {
    s.sort_unstable();
    let n = s.len();
    let sum: u128 = s.iter().sum();
    let (mn, med, mean, p95) = (s[0], s[n / 2], sum / n as u128, s[(n * 95 / 100).min(n - 1)]);
    let mops = if med > 0 { input as f64 / med as f64 * 1e9 / 1e6 } else { 0.0 };
    let status = if check_ok { "ok" } else { "invalid" };
    println!(
        "1,{run_id},{ts},{bench},time,,{input},,{input},rust,{ver},chrono,0.4,-O3 lto,{warmup},{trials},\
{mn},{med},{mean},{p95},{mops:.2},Mop/s,{rss},-1,{alloc},-1,-1,{status},{check},,",
        run_id = run_id, ts = ts, bench = bench, input = input, ver = env("PROFILE_LANG_VERSION", ""),
        warmup = warmup, trials = trials, mn = mn, med = med, mean = mean, p95 = p95,
        mops = mops, rss = peak_rss_kb(), alloc = _la(), status = status, check = check_ok);
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
    _alloc_of(|| { let _ = f(); });
    (s, ok)
}

fn main() {
    let run_id = env("PROFILE_RUN_ID", "local");
    let ts = env("PROFILE_RUN_TS", "");
    let warmup: usize = env("PROFILE_WARMUP", "3").parse().unwrap_or(3);
    let trials: usize = env("PROFILE_TRIALS", "10").parse().unwrap_or(10);

    // instant-arith
    let (s, ok) = bench(warmup, trials, || {
        let mut sum = 0i64;
        for i in 0..N_INSTANT as i64 {
            let t = DateTime::from_timestamp(i, 0).unwrap();
            let t2 = t + TimeDelta::seconds(7);
            sum += t2.timestamp();
        }
        sum
    }, |r| r == INSTANT_REF);
    emit(&run_id, &ts, "time-instant-arith", N_INSTANT, warmup, trials, s, ok);

    // localdate-arith
    let epoch = NaiveDate::from_ymd_opt(1970, 1, 1).unwrap();
    let (s, ok) = bench(warmup, trials, || {
        let mut sum = 0i64;
        for i in 0..N_DATE as u64 {
            let d = epoch + Days::new(i);
            let d2 = d + Days::new(1);
            sum += (d2 - epoch).num_days();
        }
        sum
    }, |r| r == DATE_REF);
    emit(&run_id, &ts, "time-localdate-arith", N_DATE, warmup, trials, s, ok);
}
