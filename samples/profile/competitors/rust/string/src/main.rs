// Rust string competitors — the same four workloads the Cajeta side runs over a
// ~360 KB ASCII text (base "the quick brown fox jumps over the lazy dog " × 8192):
// string-search (absent needle), string-replace (fox→feline), string-uppercase,
// and string-build-concat (build a 4000-char string). One schema-conformant CSV
// row per (benchmark, library). Cross-checks mirror Cajeta's. Libraries: stdlib
// str/String, plus memchr::memmem for the search (SIMD SOTA).
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

const BASE: &str = "the quick brown fox jumps over the lazy dog ";
const REPEAT: usize = 8192;
const PIECE: &str = "abcdefgh";
const BUILD_ITERS: usize = 500;

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

fn emit(run_id: &str, ts: &str, bench: &str, lib: &str, bytes: usize,
        warmup: usize, trials: usize, mut s: Vec<u128>, check_ok: bool) {
    s.sort_unstable();
    let n = s.len();
    let sum: u128 = s.iter().sum();
    let (mn, med, mean, p95) = (s[0], s[n / 2], sum / n as u128, s[(n * 95 / 100).min(n - 1)]);
    let mbps = if med > 0 { bytes as f64 / med as f64 * 1e9 / 1_048_576.0 } else { 0.0 };
    let status = if check_ok { "ok" } else { "invalid" };
    println!(
        "1,{run_id},{ts},{bench},string,,{bytes},,{bytes},rust,{ver},{lib},std,-O3 lto,{warmup},{trials},\
{mn},{med},{mean},{p95},{mbps:.1},MB/s,{rss},-1,{alloc},-1,-1,{status},{check},,",
        run_id = run_id, ts = ts, bench = bench, bytes = bytes, ver = env("PROFILE_LANG_VERSION", ""),
        lib = lib, warmup = warmup, trials = trials, mn = mn, med = med, mean = mean, p95 = p95,
        mbps = mbps, rss = peak_rss_kb(), alloc = _la(), status = status, check = check_ok);
}

fn bench<T, F: FnMut() -> T, C: Fn(&T) -> bool>(warmup: usize, trials: usize, mut f: F, check: C)
    -> (Vec<u128>, bool) {
    for _ in 0..warmup { let r = f(); std::hint::black_box(&r); }
    let mut s = Vec::with_capacity(trials);
    let mut ok = true;
    for _ in 0..trials {
        let t = Instant::now();
        let r = f();
        s.push(t.elapsed().as_nanos());
        ok = check(&r);
        std::hint::black_box(&r);
    }
    _alloc_of(|| { let _ = f(); });
    (s, ok)
}

fn main() {
    let run_id = env("PROFILE_RUN_ID", "local");
    let ts = env("PROFILE_RUN_TS", "");
    let warmup: usize = env("PROFILE_WARMUP", "3").parse().unwrap_or(3);
    let trials: usize = env("PROFILE_TRIALS", "10").parse().unwrap_or(10);

    let text: String = BASE.repeat(REPEAT);
    let nbytes = text.len(); // 360448
    let needle = "zzzzqx-not-present";

    // string-search (std str::find) — absent needle ⇒ None
    let (s, ok) = bench(warmup, trials, || text.find(needle), |r: &Option<usize>| r.is_none());
    emit(&run_id, &ts, "string-search", "std-find", nbytes, warmup, trials, s, ok);

    // string-search (memchr memmem) — the SIMD SOTA
    let finder = memchr::memmem::Finder::new(needle.as_bytes());
    let (s, ok) = bench(warmup, trials, || finder.find(text.as_bytes()),
                        |r: &Option<usize>| r.is_none());
    emit(&run_id, &ts, "string-search", "memchr", nbytes, warmup, trials, s, ok);

    // string-replace (std str::replace) — fox→feline; output grows
    let (s, ok) = bench(warmup, trials, || text.replace("fox", "feline"),
                        |r: &String| r.len() > nbytes);
    emit(&run_id, &ts, "string-replace", "std-replace", nbytes, warmup, trials, s, ok);

    // string-uppercase (std to_uppercase) — ASCII text ⇒ same length
    let (s, ok) = bench(warmup, trials, || text.to_uppercase(),
                        |r: &String| r.len() == nbytes);
    emit(&run_id, &ts, "string-uppercase", "std-to_uppercase", nbytes, warmup, trials, s, ok);

    // string-build-concat (String push_str builder) — build 4000 chars
    let (s, ok) = bench(warmup, trials, || {
        let mut b = String::new();
        for _ in 0..BUILD_ITERS { b.push_str(PIECE); }
        b
    }, |r: &String| r.len() == PIECE.len() * BUILD_ITERS);
    emit(&run_id, &ts, "string-build-concat", "String::push_str", PIECE.len() * BUILD_ITERS,
         warmup, trials, s, ok);
}
