// Rust JSON throughput vs Cajeta's SIMD binding deserializer.
//
// Methodology mirrors the Cajeta bench (bench/src/bench/BindBench.cajeta):
//   * warm up, then time a fixed iteration count, report best (peak) MB/s.
//   * "structural skip-all" is the headline analog of Cajeta parse<BBEmpty>:
//     fully scan + validate JSON structure, decode/materialize nothing.
//       - serde_json: from_slice::<IgnoredAny>  (scalar; borrows input)
//       - simd-json : to_tape (SIMD; MUTATES input -> fresh copy per iter,
//                     copy-only baseline subtracted, exactly as the Cajeta
//                     bench subtracts its fresh-copy baseline).
//   * DOM modes included for context (serde_json::Value / simd_json owned).
use serde::de::IgnoredAny;
use std::time::Instant;

fn time_it<F: FnMut()>(iters: u32, mut f: F) -> f64 {
    // best-of several batches; returns seconds for the fastest single iter
    let mut best = f64::MAX;
    for _ in 0..5 {
        let t = Instant::now();
        for _ in 0..iters {
            f();
        }
        let per = t.elapsed().as_secs_f64() / iters as f64;
        if per < best {
            best = per;
        }
    }
    best
}

fn mbps(bytes: usize, secs: f64) -> f64 {
    (bytes as f64 / (1024.0 * 1024.0)) / secs
}

fn main() {
    let files = ["twitter", "citm_catalog", "canada"];
    let iters = 200u32;

    println!(
        "{:<14} {:>10} {:>14} {:>14} {:>14} {:>14}",
        "dataset", "bytes", "serde-skip", "simdjson-tape", "serde-DOM", "simdjson-DOM"
    );
    println!("{}", "-".repeat(86));

    for name in files {
        let path = format!("/tmp/jsonbench/{name}.json");
        let data = std::fs::read(&path).expect("read dataset");
        let n = data.len();

        // --- serde_json structural skip-all (IgnoredAny), borrows input ---
        let serde_skip = {
            let s = time_it(iters, || {
                let _: IgnoredAny = serde_json::from_slice(&data).unwrap();
            });
            mbps(n, s)
        };

        // --- simd-json structural (to_tape), mutates -> fresh copy/iter,
        //     subtract copy-only baseline (matches Cajeta methodology) ---
        let simd_tape = {
            let copy_secs = time_it(iters, || {
                let c = data.clone();
                std::hint::black_box(&c);
            });
            let copy_parse_secs = time_it(iters, || {
                let mut c = data.clone();
                let _ = simd_json::to_tape(&mut c).unwrap();
            });
            let parse_secs = (copy_parse_secs - copy_secs).max(1e-12);
            mbps(n, parse_secs)
        };

        // --- serde_json DOM (Value) ---
        let serde_dom = {
            let s = time_it(iters, || {
                let _: serde_json::Value = serde_json::from_slice(&data).unwrap();
            });
            mbps(n, s)
        };

        // --- simd-json DOM (owned Value), copy baseline subtracted ---
        let simd_dom = {
            let copy_secs = time_it(iters, || {
                let c = data.clone();
                std::hint::black_box(&c);
            });
            let copy_parse_secs = time_it(iters, || {
                let mut c = data.clone();
                let _ = simd_json::to_owned_value(&mut c).unwrap();
            });
            let parse_secs = (copy_parse_secs - copy_secs).max(1e-12);
            mbps(n, parse_secs)
        };

        println!(
            "{:<14} {:>10} {:>13.0} {:>13.0} {:>13.0} {:>13.0}",
            name, n, serde_skip, simd_tape, serde_dom, simd_dom
        );
    }
    println!("\n(MB/s, peak-of-batches; copy baseline subtracted for simd-json which mutates input.)");
}
