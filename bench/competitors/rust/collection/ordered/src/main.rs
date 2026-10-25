// Rust collection/ competitors, ordered half: redblacktree-insert-lookup (the
// Cajeta shape), redblacktree-range, bplustree-insert, bplustree-point-lookup,
// bplustree-range, same sizes, inputs and checks as the Cajeta benchmarks.
// Library: std BTreeMap (a B-tree, the page's Rust competitor for both pages).
#[path = "../../common/collgen.rs"]
mod collgen;
use collgen::*;
use std::collections::BTreeMap;
use std::hint::black_box;

const N: usize = 1_000_000;

fn main() {
    if selected("redblacktree-insert-lookup") {
        let m = sized(N);
        let ks: Vec<i64> = (0..m as i64).map(key).collect();
        let r = bench(&mut || {
            let mut t = BTreeMap::new();
            for (i, &k) in black_box(&ks).iter().enumerate() { t.insert(k, i as i64); }
            let mut s = 0i64;
            for k in ks.iter() { s += t[k]; }
            s
        }, |&s| s == m as i64 * (m as i64 - 1) / 2);
        row("redblacktree-insert-lookup", "BTreeMap", "std", m as u64, r, &size_note(N, ""));
    }
    let keys: Vec<i64> = (0..N as i64).map(key).collect();
    if selected("bplustree-insert") {
        let r = bench(&mut || {
            let mut t = BTreeMap::new();
            for (i, &k) in black_box(&keys).iter().enumerate() { t.insert(k, i as i64); }
            (t.len(), t[&keys[7]])
        }, |&r| r == (N, 7));
        row("bplustree-insert", "BTreeMap", "std", N as u64, r, "");
    }
    let tree: BTreeMap<i64, i64> = keys.iter().enumerate().map(|(i, &k)| (k, i as i64)).collect();
    if selected("bplustree-point-lookup") {
        let idx = Gen::new().indices(10_000_000, N as i32);
        let exp: i64 = idx.iter().map(|&x| x as i64).sum();
        let q: Vec<i64> = idx.iter().map(|&i| keys[i as usize]).collect();
        let r = bench(&mut || { let mut s = 0i64; for k in black_box(&q).iter() { s += tree[k]; } s }, |&s| s == exp);
        row("bplustree-point-lookup", "BTreeMap", "std", 10_000_000, r, "");
    }
    // Ranges: 1e4 scans of 1e3 entries from the first key >= key(draw % 1e6).
    let starts: Vec<i64> = Gen::new().indices(10_000, N as i32).iter().map(|&i| key(i as i64)).collect();
    let mut sorted: Vec<(i64, i64)> = tree.iter().map(|(&k, &v)| (k, v)).collect();
    sorted.sort_unstable();
    let exp_range: i64 = starts.iter().map(|&s| {
        let p = sorted.partition_point(|e| e.0 < s);
        sorted[p..(p + 1000).min(N)].iter().map(|e| e.1).sum::<i64>()
    }).sum();
    for id in ["redblacktree-range", "bplustree-range"] {
        if selected(id) {
            let r = bench(&mut || {
                let mut s = 0i64;
                for &st in black_box(&starts).iter() { for (_, v) in tree.range(st..).take(1000) { s += *v; } }
                s
            }, |&s| s == exp_range);
            row(id, "BTreeMap", "std", 10_000_000, r, "range(start..).take(1000)");
        }
    }
}
