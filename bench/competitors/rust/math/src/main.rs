// Rust math competitors — the same three scalar f64 kernels the Cajeta side runs:
// saxpy (z = 2.5*x + y over 1M), dot-product (sum x*y over 1M), and matmul
// (200×200, A identity · B). Same init as Cajeta, so the exact reference
// checksums match (148250000.0 / 148499899 / 1980000). One schema-conformant CSV
// row per benchmark. Throughput reported in GFLOP/s. Scalar loops only — this
// isolates auto-vectorization quality on identical work; numpy (BLAS) is the
// Python competitor.
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

const N: usize = 1_000_000;
const NDIM: usize = 200;
const SAXPY_REF: f64 = 148_250_000.0;
const DOT_REF: f64 = 148_499_899.0;
const MATMUL_REF: f64 = 1_980_000.0;

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

fn emit(run_id: &str, ts: &str, bench: &str, input: usize, flops: f64,
        warmup: usize, trials: usize, mut s: Vec<u128>, check_ok: bool) {
    s.sort_unstable();
    let n = s.len();
    let sum: u128 = s.iter().sum();
    let (mn, med, mean, p95) = (s[0], s[n / 2], sum / n as u128, s[(n * 95 / 100).min(n - 1)]);
    let gflops = if med > 0 { flops / med as f64 } else { 0.0 }; // FLOP per ns == GFLOP/s
    let status = if check_ok { "ok" } else { "invalid" };
    println!(
        "1,{run_id},{ts},{bench},math,,{input},,{input},rust,{ver},scalar,std,-O3 lto target-cpu=native,{warmup},{trials},\
{mn},{med},{mean},{p95},{gflops:.3},GFLOP/s,{rss},-1,{alloc},-1,-1,{status},{check},,",
        run_id = run_id, ts = ts, bench = bench, input = input, ver = env("PROFILE_LANG_VERSION", ""),
        warmup = warmup, trials = trials, mn = mn, med = med, mean = mean, p95 = p95,
        gflops = gflops, rss = peak_rss_kb(), alloc = _la(), status = status, check = check_ok);
}

fn approx(a: f64, b: f64) -> bool { (a - b).abs() <= 1e-6 * b.abs().max(1.0) }

fn main() {
    let run_id = env("PROFILE_RUN_ID", "local");
    let ts = env("PROFILE_RUN_TS", "");
    let warmup: usize = env("PROFILE_WARMUP", "3").parse().unwrap_or(3);
    let trials: usize = env("PROFILE_TRIALS", "10").parse().unwrap_or(10);

    let x: Vec<f64> = (0..N).map(|i| (i % 100) as f64).collect();
    let y_saxpy: Vec<f64> = (0..N).map(|i| (i % 50) as f64).collect();
    let y_dot: Vec<f64> = (0..N).map(|i| (i % 7) as f64).collect();

    // saxpy: z = 2.5*x + y ; checksum = sum(z). Idiomatic iterator form (zip) so
    // the FMA store auto-vectorizes — the indexed form keeps bounds checks that
    // block it, which would misrepresent Rust.
    let a = 2.5f64;
    let mut samples = Vec::new(); let mut ok = true;
    let mut z = vec![0.0f64; N];
    let saxpy = |z: &mut [f64]| z.iter_mut().zip(&x).zip(&y_saxpy)
        .for_each(|((zi, &xi), &yi)| *zi = a * xi + yi);
    for _ in 0..warmup { saxpy(&mut z); }
    for _ in 0..trials {
        let t = Instant::now();
        saxpy(&mut z);
        let sum: f64 = z.iter().sum();
        samples.push(t.elapsed().as_nanos());
        ok = approx(sum, SAXPY_REF);
        std::hint::black_box(&z);
    }
    emit(&run_id, &ts, "saxpy", N, 2.0 * N as f64, warmup, trials, samples, ok);

    // dot-product: sum(x*y) (iterator form)
    let mut samples = Vec::new(); let mut ok = true;
    let dot = || x.iter().zip(&y_dot).map(|(&a, &b)| a * b).sum::<f64>();
    for _ in 0..warmup { std::hint::black_box(dot()); }
    for _ in 0..trials {
        let t = Instant::now();
        let s = dot();
        samples.push(t.elapsed().as_nanos());
        ok = approx(s, DOT_REF);
        std::hint::black_box(s);
    }
    emit(&run_id, &ts, "dot-product", N, 2.0 * N as f64, warmup, trials, samples, ok);

    // matmul: C = A·B, A identity, B[idx]=idx%100 ; checksum = sum(C)
    let mut am = vec![0.0f64; NDIM * NDIM];
    let mut bm = vec![0.0f64; NDIM * NDIM];
    for i in 0..NDIM { for j in 0..NDIM {
        let idx = i * NDIM + j;
        am[idx] = if i == j { 1.0 } else { 0.0 };
        bm[idx] = (idx % 100) as f64;
    }}
    let mut cm = vec![0.0f64; NDIM * NDIM];
    let matmul = |a: &[f64], b: &[f64], c: &mut [f64]| {
        for i in 0..NDIM {
            for j in 0..NDIM {
                let mut acc = 0.0;
                for k in 0..NDIM { acc += a[i * NDIM + k] * b[k * NDIM + j]; }
                c[i * NDIM + j] = acc;
            }
        }
    };
    let mut samples = Vec::new(); let mut ok = true;
    for _ in 0..warmup { matmul(&am, &bm, &mut cm); }
    for _ in 0..trials {
        let t = Instant::now();
        matmul(&am, &bm, &mut cm);
        samples.push(t.elapsed().as_nanos());
        ok = approx(cm.iter().sum(), MATMUL_REF);
        std::hint::black_box(&cm);
    }
    emit(&run_id, &ts, "matmul", NDIM * NDIM, 2.0 * (NDIM as f64).powi(3), warmup, trials, samples, ok);
}
