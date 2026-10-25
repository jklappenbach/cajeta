// math/ competitors in Rust (benchmark-reference unit 11, spec §6.8): every
// math/ workload the pages name for Rust, with the crates they name — ndarray
// (Tensor), nalgebra (LinAlg, poly-fit), rustfft (Fft), rand + rand_distr
// (Generator), npyz (Npy), statrs (Stats), glam (Ray, Rotation, Transform) —
// at the sizes, data and checks of the Cajeta side (u11 WORKLOADS). One v1 row
// (31 columns) per measurement; run.sh maps them to schema v2 math/<slug>.
// Errors propagate as a Result to the row (status failed, message in notes).
use std::time::Instant;

use glam::{Mat4, Quat, Vec3, Vec3A};
use nalgebra::{DMatrix, DVector};
use ndarray::{Array1, Array2, Axis};
use rand::rngs::StdRng;
use rand::{RngExt, SeedableRng};
use rand_distr::{Distribution, StandardNormal};
use rustfft::num_complex::Complex32;
use rustfft::FftPlanner;
use statrs::statistics::Statistics;

type R<T> = Result<T, String>;

const V_NDARRAY: &str = "0.17.2";
const V_NALGEBRA: &str = "0.35.0";
const V_RUSTFFT: &str = "6.4.1";
const V_RAND: &str = "0.10.3 (rand_distr 0.6.0)";
const V_NPYZ: &str = "0.9.1";
const V_STATRS: &str = "0.19.1";
const V_GLAM: &str = "0.34.1";
const FLAGS: &str = "-C opt-level=3 -C target-cpu=native lto";
const EP: &str = "errors propagate Result";

fn env(k: &str, d: &str) -> String {
    std::env::var(k).ok().filter(|s| !s.is_empty()).unwrap_or_else(|| d.to_string())
}

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| {
            s.lines()
                .find(|l| l.starts_with("VmHWM:"))
                .and_then(|l| l.split_whitespace().nth(1))
                .and_then(|v| v.parse().ok())
        })
        .unwrap_or(-1)
}

struct Ctx {
    run_id: String,
    ts: String,
    warmup: usize,
    trials: usize,
    smoke: bool,
    ids: Option<Vec<String>>,
    ver: String,
}

impl Ctx {
    fn want(&self, slug: &str) -> bool {
        match &self.ids {
            None => true,
            Some(v) => v.iter().any(|s| s == slug),
        }
    }

    // Warmup then timed trials of `f`; returns the sample nanoseconds.
    fn measure<F: FnMut() -> R<()>>(&self, mut f: F) -> R<Vec<u128>> {
        for _ in 0..self.warmup {
            f()?;
        }
        let mut s = Vec::with_capacity(self.trials);
        for _ in 0..self.trials {
            let t = Instant::now();
            f()?;
            s.push(t.elapsed().as_nanos());
        }
        Ok(s)
    }

    #[allow(clippy::too_many_arguments)]
    fn emit(&self, slug: &str, input: u64, lib: &str, libver: &str, variant: &str,
            mut s: Vec<u128>, ok: bool, notes: &str) {
        s.sort_unstable();
        let n = s.len().max(1);
        let sum: u128 = s.iter().sum();
        let (mn, med, mean, p95) = if s.is_empty() {
            (0, 0, 0, 0)
        } else {
            (s[0], s[n / 2], sum / n as u128, s[(n * 95 / 100).min(n - 1)])
        };
        let status = if ok { "ok" } else { "invalid" };
        println!(
            "1,{},{},{},math,,{},,{},rust,{},{},{},{},{},{},{},{},{},{},,,{},-1,-1,-1,-1,{},{},{},{}",
            self.run_id, self.ts, slug, input, input, self.ver, lib, libver, FLAGS,
            self.warmup, self.trials, mn, med, mean, p95, peak_rss_kb(), status, ok,
            clean(notes), variant
        );
    }

    fn failed(&self, slug: &str, lib: &str, libver: &str, variant: &str, msg: &str) {
        println!(
            "1,{},{},{},math,,0,,0,rust,{},{},{},{},{},{},-1,-1,-1,-1,,,{},-1,-1,-1,-1,failed,,{},{}",
            self.run_id, self.ts, slug, self.ver, lib, libver, FLAGS, self.warmup, self.trials,
            peak_rss_kb(), clean(&format!("{}; {}", msg, EP)), variant
        );
    }
}

fn clean(s: &str) -> String {
    s.replace(',', ";").replace('"', "").replace('\n', " ")
}

const P: i64 = 10007;
fn u(i: i64) -> f64 {
    ((i * 7919) % P) as f64 / 10007.0
}
fn v(i: i64) -> f64 {
    let m = i % P;
    ((m * m * 7 + i * 7919) % P) as f64 / 10007.0
}

fn smoke_note(ctx: &Ctx, what: &str) -> String {
    if ctx.smoke { format!("smoke size {}; ", what) } else { String::new() }
}

fn rel_ok(got: f64, want: f64, tol: f64) -> bool {
    (got - want).abs() <= tol * want.abs().max(1e-300)
}

// ---------------------------------------------------------------- Tensor (ndarray)

fn tensor_element_get(ctx: &Ctx) -> R<()> {
    let n = 1_000_000usize;
    let a: Array1<f64> = Array1::from_shape_fn(n, |i| i as f64 * 0.5);
    let mut last = 0.0f64;
    let s = ctx.measure(|| {
        let mut s = 0.0f64;
        for i in 0..n {
            s += a[i] as f32 as f64;
        }
        last = std::hint::black_box(s);
        Ok(())
    })?;
    let ok = last == 249_999_750_000.0;
    ctx.emit("tensor-element-get", n as u64, "ndarray", V_NDARRAY, "flat-get", s, ok,
             &format!("ndarray Index a[i] (bounds-checked) narrowed to f32 and back; check: sum == 249999750000 exact; {}", EP));
    Ok(())
}

fn tensor_element_set(ctx: &Ctx) -> R<()> {
    let n = 1_000_000usize;
    let mut w: Array1<f64> = Array1::zeros(n);
    let mut t = 0.0f64;
    let s = ctx.measure(|| {
        t += 1.0;
        for i in 0..n {
            w[i] = t + i as f64;
        }
        std::hint::black_box(&w);
        Ok(())
    })?;
    let ok = w[n - 1] == t + (n - 1) as f64;
    ctx.emit("tensor-element-set", n as u64, "ndarray", V_NDARRAY, "", s, ok,
             &format!("ndarray IndexMut w[i] = t + i; check: w[n-1] == t + n-1 exact; {}", EP));
    Ok(())
}

fn tensor_elementwise(ctx: &Ctx) -> R<()> {
    let n: usize = if ctx.smoke { 1_000_000 } else { 10_000_000 };
    let a = Array1::from_shape_fn(n, |i| u(i as i64) as f32);
    let b = Array1::from_shape_fn(n, |i| u(i as i64 + 1) as f32);
    let c = Array1::from_shape_fn(n, |i| u(i as i64 + 2) as f32);
    let reff: f64 = (0..n).map(|i| a[i] as f64 * b[i] as f64 + c[i] as f64).sum();
    let mut r: Array1<f32> = Array1::zeros(0);
    let s = ctx.measure(|| {
        r = &a * &b + &c;
        Ok(())
    })?;
    let got: f64 = r.iter().map(|&x| x as f64).sum();
    let ok = rel_ok(got, reff, 1e-5);
    ctx.emit("tensor-elementwise", n as u64, "ndarray", V_NDARRAY, "", s, ok,
             &format!("{}&a * &b + &c fresh Array1<f32>; check: rel err of sum <= 1e-5; {}",
                      smoke_note(ctx, "n=1e6 in place of 1e7"), EP));
    Ok(())
}

fn tensor_matrix(rows: usize, cols: usize) -> Array2<f32> {
    Array2::from_shape_fn((rows, cols), |(r, c)| u((r * cols + c) as i64) as f32)
}

fn tensor_broadcast(ctx: &Ctx) -> R<()> {
    let rows: usize = if ctx.smoke { 1000 } else { 10000 };
    let cols = 1000usize;
    let m = tensor_matrix(rows, cols);
    let row = Array1::from_shape_fn(cols, |c| u(c as i64 + 5) as f32);
    let mut reff = 0.0f64;
    for r in 0..rows {
        for c in 0..cols {
            reff += m[[r, c]] as f64 + row[c] as f64;
        }
    }
    let mut out: Array2<f32> = Array2::zeros((0, 0));
    let s = ctx.measure(|| {
        out = &m + &row;
        Ok(())
    })?;
    let got: f64 = out.iter().map(|&x| x as f64).sum();
    let ok = out.dim() == (rows, cols) && rel_ok(got, reff, 1e-5);
    ctx.emit("tensor-broadcast", (rows * cols) as u64, "ndarray", V_NDARRAY, "", s, ok,
             &format!("{}&m + &row (row broadcast); check: rel err of sum <= 1e-5; {}",
                      smoke_note(ctx, "rows=1000 in place of 10000"), EP));
    Ok(())
}

fn tensor_reduce(ctx: &Ctx) -> R<()> {
    let rows: usize = if ctx.smoke { 1000 } else { 10000 };
    let cols = 1000usize;
    let m = tensor_matrix(rows, cols);
    // float64 reference
    let mut s0 = vec![0.0f64; cols];
    let mut mx0 = vec![f64::NEG_INFINITY; cols];
    let mut s1 = vec![0.0f64; rows];
    let mut mx1 = vec![f64::NEG_INFINITY; rows];
    for r in 0..rows {
        for c in 0..cols {
            let x = m[[r, c]] as f64;
            s0[c] += x;
            s1[r] += x;
            mx0[c] = mx0[c].max(x);
            mx1[r] = mx1[r].max(x);
        }
    }
    let ss0: f64 = s0.iter().sum();
    let ss1: f64 = s1.iter().sum();
    let reff = ss0 + ss1 + mx0.iter().sum::<f64>() + mx1.iter().sum::<f64>()
        + s0.iter().map(|x| x / rows as f64).sum::<f64>()
        + s1.iter().map(|x| x / cols as f64).sum::<f64>();
    let mut res: Vec<Array1<f32>> = Vec::new();
    let s = ctx.measure(|| {
        let a = m.sum_axis(Axis(0));
        let b = m.sum_axis(Axis(1));
        let c = m.fold_axis(Axis(0), f32::NEG_INFINITY, |&x, &y| x.max(y));
        let d = m.fold_axis(Axis(1), f32::NEG_INFINITY, |&x, &y| x.max(y));
        let e = m.mean_axis(Axis(0)).ok_or("mean_axis(0) on empty axis")?;
        let f = m.mean_axis(Axis(1)).ok_or("mean_axis(1) on empty axis")?;
        res = vec![a, b, c, d, e, f];
        Ok(())
    })?;
    let got: f64 = res.iter().map(|v| v.iter().map(|&x| x as f64).sum::<f64>()).sum();
    let ok = rel_ok(got, reff, 1e-3);
    ctx.emit("tensor-reduce", (rows * cols) as u64, "ndarray", V_NDARRAY, "", s, ok,
             &format!("{}sum_axis + fold_axis(max; ndarray has no max_axis) + mean_axis along axis 0 and 1; check: rel err of checksum <= 1e-3; {}",
                      smoke_note(ctx, "rows=1000 in place of 10000"), EP));
    Ok(())
}

fn tensor_transpose(ctx: &Ctx) -> R<()> {
    let n: usize = if ctx.smoke { 1024 } else { 4096 };
    let m = tensor_matrix(n, n);
    let mut t: Array2<f32> = Array2::zeros((0, 0));
    let s = ctx.measure(|| {
        t = m.t().as_standard_layout().into_owned();
        Ok(())
    })?;
    let mut ok = t.is_standard_layout() && t.dim() == (n, n);
    if ok {
        'o: for i in 0..n {
            for j in 0..n {
                if t[[j, i]] != m[[i, j]] {
                    ok = false;
                    break 'o;
                }
            }
        }
    }
    ctx.emit("tensor-transpose", (n * n) as u64, "ndarray", V_NDARRAY, "", s, ok,
             &format!("{}m.t().as_standard_layout().into_owned() (C-order copy); check: T[j][i] == M[i][j] exact; {}",
                      smoke_note(ctx, "N=1024 in place of 4096"), EP));
    Ok(())
}

fn tensor_cast(ctx: &Ctx) -> R<()> {
    let n: usize = if ctx.smoke { 1_000_000 } else { 10_000_000 };
    let x = Array1::from_shape_fn(n, |i| (u(i as i64) * 1000.0 - 500.0) as f32);
    let reff: i64 = x.iter().map(|&v| (v as f64).trunc() as i64).sum();
    let mut z: Array1<i32> = Array1::zeros(0);
    let s = ctx.measure(|| {
        let y: Array1<f64> = x.mapv(|v| v as f64);
        z = y.mapv(|v| v as i32);
        std::hint::black_box(&y);
        Ok(())
    })?;
    let got: i64 = z.iter().map(|&v| v as i64).sum();
    let ok = got == reff;
    ctx.emit("tensor-cast", n as u64, "ndarray", V_NDARRAY, "", s, ok,
             &format!("{}mapv(as f64) then mapv(as i32) truncating; check: int64 sum exact; {}",
                      smoke_note(ctx, "n=1e6 in place of 1e7"), EP));
    Ok(())
}

// ---------------------------------------------------------------- LinAlg (nalgebra)

fn lin_matrix(n: usize) -> DMatrix<f64> {
    DMatrix::from_fn(n, n, |i, j| {
        let mut x = u((i * n + j) as i64) - 0.5;
        if i == j {
            x += n as f64;
        }
        x
    })
}

fn max_abs(m: &DMatrix<f64>) -> f64 {
    m.iter().fold(0.0f64, |a, &x| a.max(x.abs()))
}

fn norm_inf(m: &DMatrix<f64>) -> f64 {
    (0..m.nrows()).map(|i| m.row(i).iter().map(|x| x.abs()).sum::<f64>()).fold(0.0, f64::max)
}

fn linalg(ctx: &Ctx, slug: &str) {
    for variant in ["512", "2048"] {
        let full: usize = variant.parse().unwrap();
        let n = if ctx.smoke {
            match (slug, full) {
                ("linalg-svd", 512) => 64,
                ("linalg-svd", _) => 128,
                (_, 512) => 128,
                _ => 256,
            }
        } else {
            full
        };
        let sn = smoke_note(ctx, &format!("n={} in place of {}", n, full));
        if let Err(e) = linalg_one(ctx, slug, variant, n, &sn) {
            ctx.failed(slug, "nalgebra", V_NALGEBRA, variant, &e);
        }
    }
}

fn linalg_one(ctx: &Ctx, slug: &str, variant: &str, n: usize, sn: &str) -> R<()> {
    let a = lin_matrix(n);
    let b = DVector::from_fn(n, |i, _| u(i as i64 + 3));
    let id = DMatrix::<f64>::identity(n, n);
    let (s, ok, note) = match slug {
        "linalg-solve" => {
            let mut x = DVector::zeros(n);
            let s = ctx.measure(|| {
                x = a.clone().lu().solve(&b).ok_or("LU solve: singular matrix")?;
                Ok(())
            })?;
            let r = &a * &x - &b;
            let res = r.amax() / (norm_inf(&a) * x.amax() + b.amax());
            (s, res <= 1e-12, format!("A.clone().lu().solve(&b) (partial pivoting); check: residual {:.3e} <= 1e-12", res))
        }
        "linalg-inverse" => {
            let mut ai = DMatrix::zeros(n, n);
            let s = ctx.measure(|| {
                ai = a.clone().try_inverse().ok_or("try_inverse: singular matrix")?;
                Ok(())
            })?;
            let e = max_abs(&(&a * &ai - &id));
            (s, e <= 1e-10, format!("A.clone().try_inverse(); check: max|A*Ai - I| {:.3e} <= 1e-10", e))
        }
        "linalg-qr" => {
            let mut q = DMatrix::zeros(n, n);
            let mut rr = DMatrix::zeros(n, n);
            let s = ctx.measure(|| {
                let qr = a.clone().qr();
                q = qr.q();
                rr = qr.r();
                Ok(())
            })?;
            let e = max_abs(&(&q * &rr - &a)) / max_abs(&a);
            (s, e <= 1e-12, format!("A.clone().qr() with q() and r() materialized (Householder); check: max|QR - A|/max|A| {:.3e} <= 1e-12", e))
        }
        _ => {
            let mut uu = DMatrix::zeros(n, n);
            let mut vt = DMatrix::zeros(n, n);
            let mut sv = DVector::zeros(n);
            let s = ctx.measure(|| {
                let svd = a.clone().svd(true, true);
                uu = svd.u.ok_or("svd: U not computed")?;
                vt = svd.v_t.ok_or("svd: V^T not computed")?;
                sv = svd.singular_values;
                Ok(())
            })?;
            let e = max_abs(&(&uu * DMatrix::from_diagonal(&sv) * &vt - &a)) / max_abs(&a);
            (s, e <= 1e-10, format!("A.clone().svd(true; true) (U and V^T); check: max|U S Vt - A|/max|A| {:.3e} <= 1e-10", e))
        }
    };
    ctx.emit(slug, n as u64, "nalgebra", V_NALGEBRA, variant, s, ok, &format!("{}{}; {}", sn, note, EP));
    Ok(())
}

// ---------------------------------------------------------------- Fft (rustfft)

fn signal(n: usize) -> Vec<Complex32> {
    (0..n)
        .map(|k| Complex32::new((u(2 * k as i64) - 0.5) as f32, (u(2 * k as i64 + 1) - 0.5) as f32))
        .collect()
}

fn max_err(a: &[Complex32], b: &[Complex32]) -> f64 {
    a.iter().zip(b).map(|(x, y)| ((x.re - y.re).abs().max((x.im - y.im).abs())) as f64).fold(0.0, f64::max)
}

// Direct O(N^2) DFT at N=64 in float64 vs rustfft's forward transform.
fn dft_small_err(planner: &mut FftPlanner<f32>) -> f64 {
    let n = 64usize;
    let x = signal(n);
    let mut y = x.clone();
    planner.plan_fft_forward(n).process(&mut y);
    let mut e = 0.0f64;
    for k in 0..n {
        let (mut re, mut im) = (0.0f64, 0.0f64);
        for (t, xt) in x.iter().enumerate() {
            let th = -2.0 * std::f64::consts::PI * (k * t) as f64 / n as f64;
            re += xt.re as f64 * th.cos() - xt.im as f64 * th.sin();
            im += xt.re as f64 * th.sin() + xt.im as f64 * th.cos();
        }
        e = e.max((re - y[k].re as f64).abs()).max((im - y[k].im as f64).abs());
    }
    e
}

fn fft_1d(ctx: &Ctx) {
    for (variant, full) in [("2^16", 65536usize), ("2^20", 1usize << 20)] {
        let n = if ctx.smoke && full == (1 << 20) { 262144 } else { full };
        let sn = if n != full { smoke_note(ctx, &format!("N={} in place of {}", n, full)) } else { String::new() };
        let r: R<()> = (|| {
            let mut planner = FftPlanner::<f32>::new();
            let e64 = dft_small_err(&mut planner);
            let x = signal(n);
            let fwd = planner.plan_fft_forward(n);
            let inv = planner.plan_fft_inverse(n);
            let mut scratch = vec![Complex32::new(0.0, 0.0); fwd.get_inplace_scratch_len().max(inv.get_inplace_scratch_len())];
            let mut buf = x.clone();
            let s = ctx.measure(|| {
                buf.copy_from_slice(&x);
                fwd.process_with_scratch(&mut buf, &mut scratch);
                Ok(())
            })?;
            let mut back = buf.clone();
            inv.process_with_scratch(&mut back, &mut scratch);
            let sc = 1.0 / n as f32;
            back.iter_mut().for_each(|c| *c *= sc);
            let rt = max_err(&back, &x);
            let ok = e64 <= 1e-4 && rt <= 1e-4;
            ctx.emit("fft-1d", n as u64, "rustfft", V_RUSTFFT, variant, s, ok,
                     &format!("{}Complex32 in-place forward on a copy of x (copy timed; planner outside timing); check: N=64 direct DFT max err {:.2e} <= 1e-4 and round trip max err {:.2e} <= 1e-4; {}",
                              sn, e64, rt, EP));
            Ok(())
        })();
        if let Err(e) = r {
            ctx.failed("fft-1d", "rustfft", V_RUSTFFT, variant, &e);
        }
    }
}

fn transpose_c(src: &[Complex32], dst: &mut [Complex32], rows: usize, cols: usize) {
    const B: usize = 32;
    for ib in (0..rows).step_by(B) {
        for jb in (0..cols).step_by(B) {
            for i in ib..(ib + B).min(rows) {
                for j in jb..(jb + B).min(cols) {
                    dst[j * rows + i] = src[i * cols + j];
                }
            }
        }
    }
}

fn fft_2d(ctx: &Ctx) -> R<()> {
    let n: usize = if ctx.smoke { 512 } else { 2048 };
    let x = signal(n * n);
    let mut planner = FftPlanner::<f32>::new();
    let fwd = planner.plan_fft_forward(n);
    let inv = planner.plan_fft_inverse(n);
    let mut scratch = vec![Complex32::new(0.0, 0.0); fwd.get_inplace_scratch_len().max(inv.get_inplace_scratch_len())];
    let mut buf = x.clone();
    let mut tmp = x.clone();
    let mut run2d = |buf: &mut Vec<Complex32>, tmp: &mut Vec<Complex32>, f: &std::sync::Arc<dyn rustfft::Fft<f32>>| {
        f.process_with_scratch(buf, &mut scratch); // every row
        transpose_c(buf, tmp, n, n);
        f.process_with_scratch(tmp, &mut scratch); // every column (as rows)
        transpose_c(tmp, buf, n, n);
    };
    let s = ctx.measure(|| {
        buf.copy_from_slice(&x);
        run2d(&mut buf, &mut tmp, &fwd);
        Ok(())
    })?;
    let mut back = buf.clone();
    run2d(&mut back, &mut tmp, &inv);
    let sc = 1.0 / (n * n) as f32;
    back.iter_mut().for_each(|c| *c *= sc);
    let rt = max_err(&back, &x);
    let ok = rt <= 1e-4;
    ctx.emit("fft-2d", (n * n) as u64, "rustfft", V_RUSTFFT, "", s, ok,
             &format!("{}rustfft has no 2-D API: row FFTs + blocked transpose + row FFTs + transpose (Complex32; copy of x timed); check: round trip max err {:.2e} <= 1e-4; {}",
                      smoke_note(ctx, "512x512 in place of 2048x2048"), rt, EP));
    Ok(())
}

// ---------------------------------------------------------------- Random (rand)

fn mean_var(x: &[f64]) -> (f64, f64) {
    let n = x.len() as f64;
    let m = x.iter().sum::<f64>() / n;
    let v = x.iter().map(|&a| (a - m) * (a - m)).sum::<f64>() / n;
    (m, v)
}

fn random(ctx: &Ctx, normal: bool) -> R<()> {
    let n: usize = if ctx.smoke { 10_000_000 } else { 100_000_000 };
    let mut rng = StdRng::seed_from_u64(42);
    let mut buf = vec![0.0f64; n];
    let s = ctx.measure(|| {
        if normal {
            for x in buf.iter_mut() {
                *x = StandardNormal.sample(&mut rng);
            }
        } else {
            for x in buf.iter_mut() {
                *x = rng.random::<f64>();
            }
        }
        Ok(())
    })?;
    let (m, v) = mean_var(&buf);
    let sn = smoke_note(ctx, "n=1e7 in place of 1e8");
    if normal {
        let ok = m.abs() <= 1e-3 && (v - 1.0).abs() <= 2e-3;
        ctx.emit("random-normal", n as u64, "rand", V_RAND, "", s, ok,
                 &format!("{}StdRng (ChaCha12) seed_from_u64(42) + rand_distr StandardNormal into a preallocated Vec<f64>; check: |mean| {:.1e} <= 1e-3 and |var-1| {:.1e} <= 2e-3; {}",
                          sn, m.abs(), (v - 1.0).abs(), EP));
    } else {
        let ok = (m - 0.5).abs() <= 1e-3 && (v - 1.0 / 12.0).abs() <= 1e-3;
        ctx.emit("random-uniform", n as u64, "rand", V_RAND, "", s, ok,
                 &format!("{}StdRng (ChaCha12) seed_from_u64(42) random::<f64>() into a preallocated Vec<f64>; check: |mean-0.5| {:.1e} <= 1e-3 and |var-1/12| {:.1e} <= 1e-3; {}",
                          sn, (m - 0.5).abs(), (v - 1.0 / 12.0).abs(), EP));
    }
    Ok(())
}

// ---------------------------------------------------------------- Npy (npyz)

const NPY_N: usize = 8388608;

fn npy_data() -> Vec<i32> {
    let mut t = vec![0i32; NPY_N];
    t[0] = 7;
    t[NPY_N - 1] = 8247;
    t
}

// The reference image NumPy's np.save writes for npy_data(): 128-byte v1.0 header + LE data.
fn npy_reference(t: &[i32]) -> Vec<u8> {
    let mut h = format!("{{'descr': '<i4', 'fortran_order': False, 'shape': ({},), }}", t.len()).into_bytes();
    while (10 + h.len() + 1) % 64 != 0 {
        h.push(b' ');
    }
    h.push(b'\n');
    let mut img = Vec::with_capacity(10 + h.len() + 4 * t.len());
    img.extend_from_slice(b"\x93NUMPY\x01\x00");
    img.extend_from_slice(&(h.len() as u16).to_le_bytes());
    img.extend_from_slice(&h);
    for v in t {
        img.extend_from_slice(&v.to_le_bytes());
    }
    img
}

fn npy_load(ctx: &Ctx) -> R<()> {
    let t = npy_data();
    let img = npy_reference(&t);
    let mut back: Vec<i32> = Vec::new();
    let s = ctx.measure(|| {
        let f = npyz::NpyFile::new(&img[..]).map_err(|e| e.to_string())?;
        back = f.into_vec::<i32>().map_err(|e| e.to_string())?;
        Ok(())
    })?;
    let sum: i64 = back.iter().map(|&x| x as i64).sum();
    let ok = back.len() == NPY_N && sum == 8254 && back[0] == 7 && back[NPY_N - 1] == 8247;
    ctx.emit("npy-load-i32", img.len() as u64, "npyz", V_NPYZ, "", s, ok,
             &format!("NpyFile::new(&img[..]).into_vec::<i32>() from the in-memory NumPy image (no disk); check: len 8388608 and sum 8254 and ends 7/8247; {}", EP));
    Ok(())
}

fn npy_save(ctx: &Ctx) -> R<()> {
    use npyz::WriterBuilder;
    let t = npy_data();
    let reff = npy_reference(&t);
    let mut out: Vec<u8> = Vec::new();
    let s = ctx.measure(|| {
        let mut buf: Vec<u8> = Vec::with_capacity(reff.len());
        {
            let mut w = npyz::WriteOptions::new()
                .default_dtype()
                .shape(&[NPY_N as u64])
                .writer(&mut buf)
                .begin_nd()
                .map_err(|e| e.to_string())?;
            w.extend(t.iter().copied()).map_err(|e| e.to_string())?;
            w.finish().map_err(|e| e.to_string())?;
        }
        out = buf;
        Ok(())
    })?;
    // Full byte equality with np.save's image is recorded; the check is the
    // payload byte-equal plus a valid v1.0 header that parses to the same
    // descr/order/shape (npyz spells the shape tuple '(8388608, )').
    let full_eq = out == reff;
    let hl = |b: &[u8]| if b.len() >= 10 { 10 + u16::from_le_bytes([b[8], b[9]]) as usize } else { b.len() };
    let (ho, hr) = (hl(&out), hl(&reff));
    let v10 = out.len() >= 10 && &out[0..8] == b"\x93NUMPY\x01\x00" && ho % 64 == 0;
    let payload_eq = out.len() >= ho && out[ho..] == reff[hr..];
    let hdr_ok = match npyz::NpyFile::new(&out[..]) {
        Ok(f) => f.dtype().descr() == "'<i4'" && f.order() == npyz::Order::C && f.shape() == [NPY_N as u64],
        Err(_) => false,
    };
    let ok = v10 && payload_eq && hdr_ok;
    let eq_note = if full_eq {
        String::from("byte equality with np.save holds")
    } else {
        let hdr = String::from_utf8_lossy(&out[10.min(out.len())..ho.min(out.len())]).trim_end().to_string();
        format!("byte equality with np.save fails: npyz header is [{}] (shape written '(8388608; )' vs NumPy '(8388608;)': one header byte; same length {})", hdr, out.len() == reff.len())
    };
    let note = format!("npyz WriteOptions default_dtype shape([n]) writer(Vec<u8>) begin_nd + extend; {}; check: payload byte-equal ({}) + v1.0 header parses equal: descr '<i4' fortran_order False shape (8388608;) ({})",
                       eq_note, payload_eq, hdr_ok && v10);
    ctx.emit("npy-save-i32", reff.len() as u64, "npyz", V_NPYZ, "", s, ok, &format!("{}; {}", note, EP));
    Ok(())
}

// ---------------------------------------------------------------- Poly

const COEF: [f64; 9] = [0.5, -1.25, 2.0, -0.75, 1.5, -2.25, 0.25, 1.75, -1.0];

fn horner(x: f64) -> f64 {
    let mut y = 0.0f64;
    for &c in COEF.iter() {
        y = y * x + c;
    }
    y
}

fn poly_eval(ctx: &Ctx) -> R<()> {
    let n: usize = if ctx.smoke { 10_000_000 } else { 100_000_000 };
    let x: Vec<f64> = (0..n).map(|i| i as f64 / n as f64).collect();
    let mut y = vec![0.0f64; n];
    let s = ctx.measure(|| {
        for (yi, &xi) in y.iter_mut().zip(&x) {
            *yi = horner(xi);
        }
        Ok(())
    })?;
    let integral: f64 = COEF.iter().enumerate().map(|(k, c)| c / (9 - k) as f64).sum();
    let p1: f64 = COEF.iter().sum();
    let p0 = COEF[8];
    let reff = n as f64 * integral - (p1 - p0) / 2.0;
    let got: f64 = y.iter().sum();
    let ok = rel_ok(got, reff, 1e-7);
    ctx.emit("poly-eval", n as u64, "std", "-", "", s, ok,
             &format!("{}plain Horner (no Rust polynomial crate named) into a Vec<f64>; check: rel err of sum vs Euler-Maclaurin {:.1e} <= 1e-7; {}",
                      smoke_note(ctx, "n=1e7 in place of 1e8"), ((got - reff) / reff).abs(), EP));
    Ok(())
}

fn poly_fit(ctx: &Ctx) -> R<()> {
    let (m, fits): (usize, usize) = if ctx.smoke { (100_000, 10) } else { (1_000_000, 100) };
    let x: Vec<f64> = (0..m).map(|i| -1.0 + 2.0 * i as f64 / (m - 1) as f64).collect();
    let y = DVector::from_iterator(m, x.iter().map(|&xi| horner(xi)));
    let mut coef = DVector::<f64>::zeros(9);
    let s = ctx.measure(|| {
        for _ in 0..fits {
            let v = DMatrix::from_fn(m, 9, |i, j| x[i].powi(8 - j as i32));
            let qr = v.qr();
            let mut rhs = y.clone();
            qr.q_tr_mul(&mut rhs);
            let r = qr.r();
            coef = r.solve_upper_triangular(&rhs.rows(0, 9).into_owned()).ok_or("QR: singular R")?;
        }
        Ok(())
    })?;
    let e = (0..9).map(|k| (coef[k] - COEF[k]).abs()).fold(0.0, f64::max);
    let ok = e <= 1e-6;
    ctx.emit("poly-fit", m as u64, "nalgebra", V_NALGEBRA, "", s, ok,
             &format!("{}{} fits per trial; each builds the m x 9 Vandermonde DMatrix and solves by Householder QR (q_tr_mul + solve_upper_triangular); check: max|coef - c| {:.1e} <= 1e-6; {}",
                      smoke_note(ctx, "m=1e5 and 10 fits in place of 1e6 and 100"), fits, e, EP));
    Ok(())
}

// ---------------------------------------------------------------- Stats (statrs)

fn stats_data(n: usize) -> Vec<f64> {
    (0..n).map(|i| u(i as i64)).collect()
}

fn stats_moments(ctx: &Ctx) -> R<()> {
    let n: usize = if ctx.smoke { 10_000_000 } else { 100_000_000 };
    let x = stats_data(n);
    let mut out = (0.0f64, 0.0f64, 0.0f64, 0.0f64);
    let s = ctx.measure(|| {
        let mean = x.iter().mean();
        let var = x.iter().population_variance();
        let (mut m3, mut m4) = (0.0f64, 0.0f64);
        for &a in &x {
            let d = a - mean;
            let d2 = d * d;
            m3 += d2 * d;
            m4 += d2 * d2;
        }
        m3 /= n as f64;
        m4 /= n as f64;
        out = (mean, var, m3 / var.powf(1.5), m4 / (var * var) - 3.0);
        Ok(())
    })?;
    let (m, v, sk, ku) = out;
    let ok = (m - 0.49995).abs() <= 1e-3 && (v - 1.0 / 12.0).abs() <= 1e-3 && sk.abs() <= 1e-3 && (ku + 1.2).abs() <= 1e-3;
    ctx.emit("stats-moments", n as u64, "statrs", V_STATRS, "", s, ok,
             &format!("{}statrs Statistics mean + population_variance; statrs has no sample skewness/kurtosis for data so m3/m4 are one plain pass (biased Fisher); check: |mean-0.49995| |var-1/12| |skew| |kurt+1.2| each <= 1e-3; {}",
                      smoke_note(ctx, "n=1e7 in place of 1e8"), EP));
    Ok(())
}

fn stats_histogram(ctx: &Ctx) -> R<()> {
    let n: usize = if ctx.smoke { 10_000_000 } else { 100_000_000 };
    let x = stats_data(n);
    let mut reff = vec![0u64; 1000];
    for i in 0..n as i64 {
        reff[(((i * 7919) % P) * 1000 / P) as usize] += 1;
    }
    let mut counts = vec![0u64; 1000];
    let s = ctx.measure(|| {
        let mut c = vec![0u64; 1000];
        for &a in &x {
            let mut b = (a * 1000.0) as usize;
            if b >= 1000 {
                b = 999;
            }
            c[b] += 1;
        }
        counts = c;
        Ok(())
    })?;
    let ok = counts == reff;
    ctx.emit("stats-histogram", n as u64, "std", "-", "", s, ok,
             &format!("{}statrs has no histogram: plain loop of floor(x*1000) into 1000 u64 bins (last edge closed); check: every count equals the integer reference; {}",
                      smoke_note(ctx, "n=1e7 in place of 1e8"), EP));
    Ok(())
}

// ---------------------------------------------------------------- Geometry (glam)

struct Rays {
    o: Vec<Vec3A>,
    d: Vec<Vec3A>,
}

fn rays(r: usize) -> Rays {
    let mut o = Vec::with_capacity(r);
    let mut d = Vec::with_capacity(r);
    for i in 0..r as i64 {
        o.push(Vec3A::new((v(3 * i) * 2.0 - 1.0) as f32, (v(3 * i + 1) * 2.0 - 1.0) as f32, -5.0));
        let (dx, dy, dz) = (v(3 * i + 2) * 0.2 - 0.1, v(3 * i + 7) * 0.2 - 0.1, 1.0f64);
        let l = (dx * dx + dy * dy + dz * dz).sqrt();
        d.push(Vec3A::new((dx / l) as f32, (dy / l) as f32, (dz / l) as f32));
    }
    Rays { o, d }
}

const S_OBJ: usize = 10000;

fn obj_min64(j: i64) -> (f64, f64, f64) {
    (v(5 * j) * 4.0 - 2.0, v(5 * j + 1) * 4.0 - 2.0, v(5 * j + 2) * 4.0)
}

fn ray_sphere(ctx: &Ctx) -> R<()> {
    let r: usize = if ctx.smoke { 1000 } else { 10000 };
    let reff: i64 = if ctx.smoke { 51613 } else { 502607 };
    let rs = rays(r);
    let mut c = Vec::with_capacity(S_OBJ);
    let mut rr = Vec::with_capacity(S_OBJ);
    for j in 0..S_OBJ as i64 {
        let (x, y, z) = obj_min64(j);
        c.push(Vec3A::new(x as f32, y as f32, z as f32));
        let rad = (0.05 + v(5 * j + 3) * 0.2) as f32;
        rr.push(rad * rad);
    }
    let mut hits = 0i64;
    let s = ctx.measure(|| {
        let mut h = 0i64;
        for i in 0..r {
            let (o, d) = (rs.o[i], rs.d[i]);
            for j in 0..S_OBJ {
                let oc = o - c[j];
                let b = oc.dot(d);
                let cc = oc.dot(oc) - rr[j];
                let disc = b * b - cc;
                if disc >= 0.0 && (b <= 0.0 || cc <= 0.0) {
                    h += 1;
                }
            }
        }
        hits = std::hint::black_box(h);
        Ok(())
    })?;
    let tests = (r * S_OBJ) as i64;
    let ok = ((hits - reff).abs() as f64) <= 1e-5 * tests as f64;
    ctx.emit("ray-sphere", tests as u64, "glam", V_GLAM, "", s, ok,
             &format!("{}glam Vec3A dot; the Cajeta predicate (disc >= 0 and (b <= 0 or c <= 0)); hits {} ref {}; check: |hits-ref| <= 1e-5*tests; {}",
                      smoke_note(ctx, "R=1000 (1e7 tests) in place of 10000"), hits, reff, EP));
    Ok(())
}

fn ray_aabb(ctx: &Ctx) -> R<()> {
    let r: usize = if ctx.smoke { 1000 } else { 10000 };
    let reff: i64 = if ctx.smoke { 49068 } else { 476570 };
    let rs = rays(r);
    let mut mn = Vec::with_capacity(S_OBJ);
    let mut mx = Vec::with_capacity(S_OBJ);
    for j in 0..S_OBJ as i64 {
        let (x, y, z) = obj_min64(j);
        let sd = 0.1 + v(5 * j + 3) * 0.3;
        mn.push(Vec3A::new(x as f32, y as f32, z as f32));
        mx.push(Vec3A::new((x + sd) as f32, (y + sd) as f32, (z + sd) as f32));
    }
    let mut hits = 0i64;
    let s = ctx.measure(|| {
        let mut h = 0i64;
        for i in 0..r {
            let (o, d) = (rs.o[i], rs.d[i]);
            for j in 0..S_OBJ {
                let inv = Vec3A::ONE / d;
                let ta = (mn[j] - o) * inv;
                let tb = (mx[j] - o) * inv;
                let t0 = ta.min(tb).max_element().max(0.0);
                let t1 = ta.max(tb).min_element().min(1e8);
                if t0 <= t1 {
                    h += 1;
                }
            }
        }
        hits = std::hint::black_box(h);
        Ok(())
    })?;
    let tests = (r * S_OBJ) as i64;
    let ok = ((hits - reff).abs() as f64) <= 1e-5 * tests as f64;
    ctx.emit("ray-aabb", tests as u64, "glam", V_GLAM, "", s, ok,
             &format!("{}glam Vec3A slab test (min/max per axis); hits {} ref {}; check: |hits-ref| <= 1e-5*tests; {}",
                      smoke_note(ctx, "R=1000 (1e7 tests) in place of 10000"), hits, reff, EP));
    Ok(())
}

fn rotation_compose(ctx: &Ctx) -> R<()> {
    let n: usize = if ctx.smoke { 10_000_000 } else { 100_000_000 };
    let reff = if ctx.smoke { 38600.96393095818 } else { 386090.57108821085 };
    let mut qt = Vec::with_capacity(1024);
    let mut vt = Vec::with_capacity(1024);
    for j in 0..1024i64 {
        let (ax, ay, az) = (v(4 * j) - 0.5, v(4 * j + 1) - 0.5, v(4 * j + 2) - 0.5);
        let l = (ax * ax + ay * ay + az * az).sqrt();
        let a = v(4 * j + 3) * 2.0 * std::f64::consts::PI;
        let (sh, ch) = ((a / 2.0).sin(), (a / 2.0).cos());
        qt.push(Quat::from_xyzw((ax / l * sh) as f32, (ay / l * sh) as f32, (az / l * sh) as f32, ch as f32));
        vt.push(Vec3::new((v(3 * j + 11) - 0.5) as f32, (v(3 * j + 12) - 0.5) as f32, (v(3 * j + 13) - 0.5) as f32));
    }
    let mut sum = 0.0f64;
    let s = ctx.measure(|| {
        let mut acc = 0.0f64;
        for i in 0..n {
            let q = qt[i & 1023] * qt[(i >> 10) & 1023];
            let w = q * vt[i & 1023];
            acc += w.x as f64 + w.y as f64 + w.z as f64;
        }
        sum = std::hint::black_box(acc);
        Ok(())
    })?;
    let ok = (sum - reff).abs() <= 1e-6 * n as f64;
    ctx.emit("rotation-compose", n as u64, "glam", V_GLAM, "", s, ok,
             &format!("{}glam Quat * Quat then Quat * Vec3; sum {:.6} ref {:.6}; check: |sum-ref| <= 1e-6*n; {}",
                      smoke_note(ctx, "n=1e7 in place of 1e8"), sum, reff, EP));
    Ok(())
}

fn transform_apply(ctx: &Ctx) -> R<()> {
    let m = 1_000_000usize;
    let passes: usize = if ctx.smoke { 10 } else { 100 };
    let (ax, ay, az) = (1.0f64, 2.0f64, 3.0f64);
    let l = (ax * ax + ay * ay + az * az).sqrt();
    let (sh, ch) = (0.35f64.sin(), 0.35f64.cos());
    let q = Quat::from_xyzw((ax / l * sh) as f32, (ay / l * sh) as f32, (az / l * sh) as f32, ch as f32);
    let mat = Mat4::from_scale_rotation_translation(Vec3::splat(2.0), q, Vec3::new(1.0, -2.0, 3.0));
    let p: Vec<Vec3> = (0..m as i64)
        .map(|k| Vec3::new((v(3 * k) * 10.0 - 5.0) as f32, (v(3 * k + 1) * 10.0 - 5.0) as f32, (v(3 * k + 2) * 10.0 - 5.0) as f32))
        .collect();
    let mut out = vec![Vec3::ZERO; m];
    let s = ctx.measure(|| {
        for _ in 0..passes {
            for (o, pk) in out.iter_mut().zip(&p) {
                *o = mat.transform_point3(*pk);
            }
            std::hint::black_box(&out);
        }
        Ok(())
    })?;
    let cs: f64 = out.iter().map(|w| w.x as f64 + w.y as f64 + w.z as f64).sum();
    let reff = 1910391.7256280927f64;
    let ok = (cs - reff).abs() <= 2e-5 * m as f64;
    ctx.emit("transform-apply", (m * passes) as u64, "glam", V_GLAM, "", s, ok,
             &format!("{}glam Mat4 (scale 2 rotation translation) transform_point3; 1e8 point applications as 100 passes over 1e6 points; checksum {:.4} ref {:.4}; check: |cs-ref| <= 2e-5*m; {}",
                      smoke_note(ctx, "10 passes (1e7 points) in place of 100"), cs, reff, EP));
    Ok(())
}

// ---------------------------------------------------------------- main

fn main() {
    let smoke = matches!(env("CAJETA_BENCH_SMOKE", "0").as_str(), "1" | "true" | "yes");
    let ctx = Ctx {
        run_id: env("PROFILE_RUN_ID", "local"),
        ts: env("PROFILE_RUN_TS", ""),
        warmup: env("PROFILE_WARMUP", if smoke { "1" } else { "3" }).parse().unwrap_or(3),
        trials: env("PROFILE_TRIALS", if smoke { "3" } else { "10" }).parse().unwrap_or(10).max(1),
        smoke,
        ids: std::env::var("MATH_IDS").ok().filter(|s| !s.trim().is_empty())
            .map(|s| s.split_whitespace().map(String::from).collect()),
        ver: clean(&env("PROFILE_LANG_VERSION", "")),
    };

    type F = fn(&Ctx) -> R<()>;
    let simple: [(&str, &str, &str, F); 18] = [
        ("tensor-element-get", "ndarray", V_NDARRAY, tensor_element_get),
        ("tensor-element-set", "ndarray", V_NDARRAY, tensor_element_set),
        ("tensor-elementwise", "ndarray", V_NDARRAY, tensor_elementwise),
        ("tensor-broadcast", "ndarray", V_NDARRAY, tensor_broadcast),
        ("tensor-reduce", "ndarray", V_NDARRAY, tensor_reduce),
        ("tensor-transpose", "ndarray", V_NDARRAY, tensor_transpose),
        ("tensor-cast", "ndarray", V_NDARRAY, tensor_cast),
        ("fft-2d", "rustfft", V_RUSTFFT, fft_2d),
        ("random-uniform", "rand", V_RAND, |c| random(c, false)),
        ("random-normal", "rand", V_RAND, |c| random(c, true)),
        ("npy-load-i32", "npyz", V_NPYZ, npy_load),
        ("npy-save-i32", "npyz", V_NPYZ, npy_save),
        ("poly-eval", "std", "-", poly_eval),
        ("poly-fit", "nalgebra", V_NALGEBRA, poly_fit),
        ("stats-moments", "statrs", V_STATRS, stats_moments),
        ("stats-histogram", "std", "-", stats_histogram),
        ("ray-sphere", "glam", V_GLAM, ray_sphere),
        ("ray-aabb", "glam", V_GLAM, ray_aabb),
    ];
    let order = [
        "tensor-element-get", "tensor-element-set", "tensor-elementwise", "tensor-broadcast",
        "tensor-reduce", "tensor-transpose", "tensor-cast", "linalg-solve", "linalg-inverse",
        "linalg-qr", "linalg-svd", "fft-1d", "fft-2d", "random-uniform", "random-normal",
        "npy-load-i32", "npy-save-i32", "poly-eval", "poly-fit", "stats-moments",
        "stats-histogram", "ray-sphere", "ray-aabb", "rotation-compose", "transform-apply",
    ];
    for slug in order {
        if !ctx.want(slug) {
            continue;
        }
        match slug {
            "linalg-solve" | "linalg-inverse" | "linalg-qr" | "linalg-svd" => linalg(&ctx, slug),
            "fft-1d" => fft_1d(&ctx),
            "rotation-compose" => {
                if let Err(e) = rotation_compose(&ctx) {
                    ctx.failed(slug, "glam", V_GLAM, "", &e);
                }
            }
            "transform-apply" => {
                if let Err(e) = transform_apply(&ctx) {
                    ctx.failed(slug, "glam", V_GLAM, "", &e);
                }
            }
            _ => {
                if let Some((_, lib, ver, f)) = simple.iter().find(|t| t.0 == slug) {
                    if let Err(e) = f(&ctx) {
                        ctx.failed(slug, lib, ver, "", &e);
                    }
                }
            }
        }
    }
}
