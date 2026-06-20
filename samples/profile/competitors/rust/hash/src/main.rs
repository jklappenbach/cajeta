// Rust hash competitors — bulk throughput over a deterministic 1 MiB buffer
// (buf[i] = i & 0xFF), the SMHasher bulk metric. One schema-conformant CSV row
// per algorithm (xxhash3, sha256, md5). Cross-check: the digest matches the
// reference computed independently (identical across every language).
use md5::Digest as _;
use sha2::Digest as _;
use std::time::Instant;

const N: usize = 1_048_576;
const SHA256_REF: &str = "fbbab289f7f94b25736c58be46a994c441fd02552cc6022352e3d86d2fab7c83";
const MD5_REF: &str = "c35cc7d8d91728a0cb052831bc4ef372";
const XXH3_REF: u64 = 0xd36c0e13a3df139e;

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:"))
            .and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

fn bench<F: FnMut() -> u64>(buf: &[u8], warmup: usize, trials: usize, mut f: F) -> (Vec<u128>, u64) {
    let mut last = 0u64;
    for _ in 0..warmup { last = f(); }
    let mut s = Vec::with_capacity(trials);
    for _ in 0..trials {
        let t = Instant::now();
        last = f();
        s.push(t.elapsed().as_nanos());
        std::hint::black_box(last);
    }
    let _ = buf;
    (s, last)
}

fn emit(run_id: &str, ts: &str, bench: &str, lib: &str, ver: &str, warmup: usize,
        trials: usize, mut samples: Vec<u128>, check_ok: bool) {
    samples.sort_unstable();
    let n = samples.len();
    let sum: u128 = samples.iter().sum();
    let (mn, med, mean, p95) = (samples[0], samples[n / 2], sum / n as u128,
                                samples[(n * 95 / 100).min(n - 1)]);
    let mbps = if med > 0 { N as f64 / med as f64 * 1e9 / 1_048_576.0 } else { 0.0 };
    let status = if check_ok { "ok" } else { "invalid" };
    println!(
        "1,{run_id},{ts},{bench},hash,,{N},,{N},rust,{ver_lang},{lib},{ver},-O3 lto,{warmup},{trials},\
{mn},{med},{mean},{p95},{mbps:.1},MB/s,{rss},-1,-1,-1,-1,{status},{check},,",
        run_id = run_id, ts = ts, bench = bench, N = N,
        ver_lang = env("PROFILE_LANG_VERSION", ""), lib = lib, ver = ver,
        warmup = warmup, trials = trials, mn = mn, med = med, mean = mean, p95 = p95,
        mbps = mbps, rss = peak_rss_kb(), status = status, check = check_ok);
}

fn main() {
    let run_id = env("PROFILE_RUN_ID", "local");
    let ts = env("PROFILE_RUN_TS", "");
    let warmup: usize = env("PROFILE_WARMUP", "3").parse().unwrap_or(3);
    let trials: usize = env("PROFILE_TRIALS", "10").parse().unwrap_or(10);
    let buf: Vec<u8> = (0..N).map(|i| (i & 0xFF) as u8).collect();

    // xxhash3
    let (s, h) = bench(&buf, warmup, trials, || xxhash_rust::xxh3::xxh3_64(&buf));
    emit(&run_id, &ts, "xxhash3", "xxhash-rust", "0.8", warmup, trials, s, h == XXH3_REF);

    // sha256
    let (s, _) = bench(&buf, warmup, trials, || {
        let d = sha2::Sha256::digest(&buf);
        u64::from_le_bytes(d[..8].try_into().unwrap())
    });
    let sha_ok = hex(&sha2::Sha256::digest(&buf)) == SHA256_REF;
    emit(&run_id, &ts, "sha256", "sha2", "0.10", warmup, trials, s, sha_ok);

    // md5
    let (s, _) = bench(&buf, warmup, trials, || {
        let d = md5::Md5::digest(&buf);
        u64::from_le_bytes(d[..8].try_into().unwrap())
    });
    let md5_ok = hex(&md5::Md5::digest(&buf)) == MD5_REF;
    emit(&run_id, &ts, "md5", "md-5", "0.10", warmup, trials, s, md5_ok);
}

fn hex(b: &[u8]) -> String {
    b.iter().map(|x| format!("{:02x}", x)).collect()
}
