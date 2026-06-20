// Rust codec competitors — parse the nativejson corpus to a DOM and emit one
// schema-conformant CSV row per (file, library), matching the contract in
// competitors/columns.txt. Libraries: serde_json (de-facto standard) and
// simd-json (SIMD SOTA). Cross-check: a known element count per file.
use std::time::Instant;

const FILES: &[(&str, &str, usize)] = &[
    // (dataset_name, filename, expected count)  twitter.statuses / citm.events / canada.features
    ("twitter", "twitter.json", 100),
    ("citm_catalog", "citm_catalog.json", 184),
    ("canada", "canada.json", 1),
];

fn env(k: &str, d: &str) -> String {
    std::env::var(k).unwrap_or_else(|_| d.to_string())
}

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status")
        .ok()
        .and_then(|s| {
            s.lines()
                .find(|l| l.starts_with("VmHWM:"))
                .and_then(|l| l.split_whitespace().nth(1))
                .and_then(|v| v.parse::<i64>().ok())
        })
        .unwrap_or(-1)
}

struct Stats {
    min: u128,
    median: u128,
    mean: u128,
    p95: u128,
}

fn stats(mut v: Vec<u128>) -> Stats {
    v.sort_unstable();
    let n = v.len();
    let sum: u128 = v.iter().sum();
    Stats {
        min: v[0],
        median: v[n / 2],
        mean: sum / n as u128,
        p95: v[(n * 95 / 100).min(n - 1)],
    }
}

#[allow(clippy::too_many_arguments)]
fn emit(
    run_id: &str, ts: &str, dataset: &str, bytes: usize, lib: &str, ver: &str,
    warmup: usize, trials: usize, s: &Stats, check_ok: bool, status: &str,
) {
    let mbps = if s.median > 0 {
        (bytes as f64) / (s.median as f64) * 1e9 / 1_048_576.0
    } else {
        0.0
    };
    // columns.txt order (31 cols).
    println!(
        "1,{run_id},{ts},json-dom,codec,{dataset},{bytes},,{bytes},rust,{rustver},{lib},{ver},-O3 lto,{warmup},{trials},\
{min},{median},{mean},{p95},{mbps:.1},MB/s,{rss},-1,-1,-1,-1,{status},{check},,",
        run_id = run_id, ts = ts, dataset = dataset, bytes = bytes,
        rustver = env("PROFILE_LANG_VERSION", ""), lib = lib, ver = ver,
        warmup = warmup, trials = trials,
        min = s.min, median = s.median, mean = s.mean, p95 = s.p95,
        mbps = mbps, rss = peak_rss_kb(), status = status, check = check_ok,
    );
}

fn main() {
    let data_dir = match std::env::var("PROFILE_DATA_DIR") {
        Ok(d) => d,
        Err(_) => {
            eprintln!("rust/codec: PROFILE_DATA_DIR unset — skipping");
            return;
        }
    };
    let run_id = env("PROFILE_RUN_ID", "local");
    let ts = env("PROFILE_RUN_TS", "");
    let warmup: usize = env("PROFILE_WARMUP", "3").parse().unwrap_or(3);
    let trials: usize = env("PROFILE_TRIALS", "10").parse().unwrap_or(10);

    let serde_ver = "1"; // pinned via Cargo.lock; report shows family
    let simd_ver = "0.13";

    for &(dataset, fname, expect) in FILES {
        let path = format!("{}/{}", data_dir, fname);
        let bytes = match std::fs::read(&path) {
            Ok(b) => b,
            Err(_) => continue, // dataset absent → silently skip (driver records gaps elsewhere)
        };
        let n = bytes.len();

        // serde_json
        {
            let check = serde_json::from_slice::<serde_json::Value>(&bytes)
                .ok()
                .map(|v| count(&v, dataset) == expect)
                .unwrap_or(false);
            for _ in 0..warmup {
                let _ = serde_json::from_slice::<serde_json::Value>(&bytes);
            }
            let mut samples = Vec::with_capacity(trials);
            for _ in 0..trials {
                let t = Instant::now();
                let v: serde_json::Value = serde_json::from_slice(&bytes).unwrap();
                let e = t.elapsed().as_nanos();
                std::hint::black_box(&v);
                samples.push(e);
            }
            emit(&run_id, &ts, dataset, n, "serde_json", serde_ver, warmup, trials,
                 &stats(samples), check, if check { "ok" } else { "invalid" });
        }

        // simd-json (mutates buffer in place → clone per parse)
        {
            let mut buf = bytes.clone();
            let check = simd_json::to_owned_value(&mut buf)
                .ok()
                .map(|v| count_simd(&v, dataset) == expect)
                .unwrap_or(false);
            for _ in 0..warmup {
                let mut b = bytes.clone();
                let _ = simd_json::to_owned_value(&mut b);
            }
            let mut samples = Vec::with_capacity(trials);
            for _ in 0..trials {
                let mut b = bytes.clone();
                let t = Instant::now();
                let v = simd_json::to_owned_value(&mut b).unwrap();
                let e = t.elapsed().as_nanos();
                std::hint::black_box(&v);
                samples.push(e);
            }
            emit(&run_id, &ts, dataset, n, "simd-json", simd_ver, warmup, trials,
                 &stats(samples), check, if check { "ok" } else { "invalid" });
        }
    }
}

fn count(v: &serde_json::Value, dataset: &str) -> usize {
    match dataset {
        "twitter" => v["statuses"].as_array().map(|a| a.len()).unwrap_or(0),
        "citm_catalog" => v["events"].as_object().map(|o| o.len()).unwrap_or(0),
        "canada" => v["features"].as_array().map(|a| a.len()).unwrap_or(0),
        _ => 0,
    }
}

fn count_simd(v: &simd_json::OwnedValue, dataset: &str) -> usize {
    use simd_json::prelude::*;
    let key = match dataset {
        "twitter" => "statuses",
        "citm_catalog" => "events",
        "canada" => "features",
        _ => return 0,
    };
    match v.get(key) {
        Some(x) if x.is_array() => x.as_array().map(|a| a.len()).unwrap_or(0),
        Some(x) if x.is_object() => x.as_object().map(|o| o.len()).unwrap_or(0),
        _ => 0,
    }
}
