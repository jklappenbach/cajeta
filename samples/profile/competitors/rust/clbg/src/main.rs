// Rust CLBG competitors — the same three Computer Language Benchmarks Game
// classics the Cajeta side runs, at the same sizes, verified against the
// canonical checksums: clbg-mandelbrot (800², in-set count 254099),
// clbg-fannkuch-redux (N=10, maxFlips 38 / checksum 73196), clbg-spectral-norm
// (N=100, ≈1.274224). One schema-conformant CSV row per benchmark. Pure scalar
// stdlib — the row reflects compiler codegen on the canonical algorithm.
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

const MANDEL_N: usize = 800;
const MANDEL_REF: i64 = 254_099;
const FANN_N: usize = 10;
const SPECTRAL_N: usize = 100;
const SPECTRAL_REF: f64 = 1.274_224;

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
    let status = if check_ok { "ok" } else { "invalid" };
    // clbg has no natural throughput unit — leave throughput/unit empty; the
    // report derives a rate from input_size/min_ns.
    println!(
        "1,{run_id},{ts},{bench},clbg,,{input},,{input},rust,{ver},scalar,std,-O3 lto,{warmup},{trials},\
{mn},{med},{mean},{p95},,,{rss},-1,{alloc},-1,-1,{status},{check},,",
        run_id = run_id, ts = ts, bench = bench, input = input, ver = env("PROFILE_LANG_VERSION", ""),
        warmup = warmup, trials = trials, mn = mn, med = med, mean = mean, p95 = p95,
        rss = peak_rss_kb(), alloc = _la(), status = status, check = check_ok);
}

fn bench<T: Copy, F: FnMut() -> T, C: Fn(T) -> bool>(warmup: usize, trials: usize, mut f: F, check: C)
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

fn mandelbrot(n: usize) -> i64 {
    let mut count = 0i64;
    for py in 0..n {
        let ci = 2.0 * py as f64 / n as f64 - 1.0;
        for px in 0..n {
            let cr = 2.0 * px as f64 / n as f64 - 1.5;
            let (mut zr, mut zi) = (0.0f64, 0.0f64);
            let mut member = true;
            for _ in 0..50 {
                let nzr = zr * zr - zi * zi + cr;
                let nzi = 2.0 * zr * zi + ci;
                zr = nzr; zi = nzi;
                if zr * zr + zi * zi > 4.0 { member = false; break; }
            }
            if member { count += 1; }
        }
    }
    count
}

fn fannkuch(n: usize) -> (i32, i32) {
    let mut perm = vec![0i32; n];
    let mut perm1: Vec<i32> = (0..n as i32).collect();
    let mut count = vec![0i32; n];
    let (mut max_flips, mut checksum, mut perm_count) = (0i32, 0i32, 0i32);
    let mut r = n;
    loop {
        while r != 1 { count[r - 1] = r as i32; r -= 1; }
        perm.copy_from_slice(&perm1);
        let mut flips = 0i32;
        let mut k = perm[0];
        while k != 0 {
            perm[..=k as usize].reverse();
            flips += 1;
            k = perm[0];
        }
        if flips > max_flips { max_flips = flips; }
        checksum += if perm_count % 2 == 0 { flips } else { -flips };
        perm_count += 1;
        loop {
            if r == n { return (max_flips, checksum); }
            let p0 = perm1[0];
            for i in 0..r { perm1[i] = perm1[i + 1]; }
            perm1[r] = p0;
            count[r] -= 1;
            if count[r] > 0 { break; }
            r += 1;
        }
    }
}

fn spectral_norm(n: usize) -> f64 {
    fn a(i: usize, j: usize) -> f64 { 1.0 / (((i + j) * (i + j + 1) / 2 + i + 1) as f64) }
    fn mul_av(v: &[f64], out: &mut [f64], n: usize) {
        for i in 0..n { let mut s = 0.0; for j in 0..n { s += a(i, j) * v[j]; } out[i] = s; }
    }
    fn mul_atv(v: &[f64], out: &mut [f64], n: usize) {
        for i in 0..n { let mut s = 0.0; for j in 0..n { s += a(j, i) * v[j]; } out[i] = s; }
    }
    let mut u = vec![1.0f64; n];
    let mut v = vec![0.0f64; n];
    let mut t = vec![0.0f64; n];
    for _ in 0..10 {
        mul_av(&u, &mut t, n); mul_atv(&t, &mut v, n);
        mul_av(&v, &mut t, n); mul_atv(&t, &mut u, n);
    }
    let (mut vbv, mut vv) = (0.0f64, 0.0f64);
    for i in 0..n { vbv += u[i] * v[i]; vv += v[i] * v[i]; }
    (vbv / vv).sqrt()
}

fn main() {
    let run_id = env("PROFILE_RUN_ID", "local");
    let ts = env("PROFILE_RUN_TS", "");
    let warmup: usize = env("PROFILE_WARMUP", "3").parse().unwrap_or(3);
    let trials: usize = env("PROFILE_TRIALS", "10").parse().unwrap_or(10);

    let (s, ok) = bench(warmup, trials, || mandelbrot(MANDEL_N), |r| r == MANDEL_REF);
    emit(&run_id, &ts, "clbg-mandelbrot", MANDEL_N * MANDEL_N, warmup, trials, s, ok);

    let (s, ok) = bench(warmup, trials, || fannkuch(FANN_N), |(mf, cs)| mf == 38 && cs == 73196);
    emit(&run_id, &ts, "clbg-fannkuch-redux", FANN_N, warmup, trials, s, ok);

    let (s, ok) = bench(warmup, trials, || spectral_norm(SPECTRAL_N),
                        |r| (r - SPECTRAL_REF).abs() < 1e-3);
    emit(&run_id, &ts, "clbg-spectral-norm", SPECTRAL_N, warmup, trials, s, ok);
}
