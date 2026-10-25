// Rust collection/ sort competitors: sort-int64 (random, ascending,
// descending, dups), sort-f64, sort-stable-int64 at the page shape (1e6;
// n/100 under CAJETA_BENCH_SMOKE=1), same inputs and checks as the Cajeta
// benchmarks (CollGen): random = key(i), f64 = draw / 2^31, stable = (value,
// index) pairs packed value << 20 | index, value = draw % 1e5, sorted on the
// value. sort_unstable (ipnsort), sort_by (f64 total order), sort_by_key
// (driftsort, stable). The input copy is outside the time.
#[path = "../../common/collgen.rs"]
mod collgen;
use collgen::*;

const FULL: usize = 1_000_000;

fn main() {
    let n = sized(FULL);
    let note = size_note(FULL, "");
    if selected("sort-int64") {
        for v in ["random", "ascending", "descending", "dups"] {
            let src: Vec<i64> = (0..n as i64).map(|i| match v {
                "random" => key(i), "ascending" => i, "descending" => n as i64 - 1 - i, _ => i % 16,
            }).collect();
            let m = bench_with(&mut || src.clone(), &mut |mut w: Vec<i64>| { w.sort_unstable(); w },
                |w| w.len() == n && w.windows(2).all(|p| p[0] <= p[1]));
            rowv("sort-int64", "sort_unstable", "std", n as u64, m, &note, v);
        }
    }
    if selected("sort-f64") {
        let mut g = Gen::new();
        let src: Vec<f64> = (0..n).map(|_| g.next() as f64 / 2147483648.0).collect();
        let m = bench_with(&mut || src.clone(), &mut |mut w: Vec<f64>| { w.sort_unstable_by(|a, b| a.total_cmp(b)); w },
            |w| w.len() == n && w.windows(2).all(|p| p[0] <= p[1]));
        row("sort-f64", "sort_unstable_by(total_cmp)", "std", n as u64, m, &note);
    }
    if selected("sort-stable-int64") {
        let mut g = Gen::new();
        let src: Vec<i64> = (0..n as i64).map(|i| (((g.next() % 100000) as i64) << 20) | i).collect();
        let m = bench_with(&mut || src.clone(), &mut |mut w: Vec<i64>| { w.sort_by_key(|x| *x >> 20); w },
            |w| w.len() == n && w.windows(2).all(|p| p[0] <= p[1]));
        row("sort-stable-int64", "sort_by_key (driftsort)", "std", n as u64, m,
            &size_note(FULL, "(value index) pairs packed in an i64; key on the value"));
    }
}
