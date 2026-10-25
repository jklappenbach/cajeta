// Rust collection/ competitors, hash half: hashmap-lookup-hit/-miss/-iterate,
// hashset-contains and the ImmutableMap/ImmutableSet ids, same sizes, inputs
// and checks as the Cajeta benchmarks. Libraries: std HashMap/HashSet
// (SipHash-1-3) and hashbrown 0.17 (foldhash). Immutable = the map or set
// cloned behind Arc (Rust has no frozen map type; the clone is the freeze).
#[path = "../../common/collgen.rs"]
mod collgen;
use collgen::*;
use std::hint::black_box;
use std::sync::Arc;

const N: usize = 1_000_000;
const Q: usize = 10_000_000;

macro_rules! hash_rows {
    ($lib:expr, $libset:expr, $ver:expr, $map:ty, $set:ty, $keys:expr, $hit_q:expr, $hit_exp:expr, $miss_q:expr, $half_q:expr, $half_exp:expr) => {{
        let keys: &Vec<i64> = $keys;
        let mut m: $map = Default::default();
        for (i, &k) in keys.iter().enumerate() { m.insert(k, i as i64); }
        let mut st: $set = Default::default();
        for &k in keys.iter() { st.insert(k); }
        let k7 = key(7);
        if selected("hashmap-lookup-hit") {
            let r = bench(&mut || { let mut s = 0i64; for k in black_box($hit_q).iter() { s += m[k]; } s }, |&s| s == $hit_exp);
            row("hashmap-lookup-hit", $lib, $ver, Q as u64, r, "");
        }
        if selected("hashmap-lookup-miss") {
            let r = bench(&mut || { let mut c = 0i64; for k in black_box($miss_q).iter() { if !m.contains_key(k) { c += 1; } } c }, |&c| c == Q as i64);
            row("hashmap-lookup-miss", $lib, $ver, Q as u64, r, "contains_key");
        }
        if selected("hashmap-iterate") {
            let r = bench(&mut || { let mut s = 0i64; for _ in 0..10 { for v in black_box(&m).values() { s += *v; } } s },
                |&s| s == 10 * (N as i64 * (N as i64 - 1) / 2));
            row("hashmap-iterate", $lib, $ver, Q as u64, r, "values()");
        }
        if selected("hashset-contains") {
            let r = bench(&mut || { let mut c = 0i64; for k in black_box($half_q).iter() { if st.contains(k) { c += 1; } } c }, |&c| c == $half_exp);
            row("hashset-contains", $libset, $ver, Q as u64, r, "");
        }
        if selected("immutablemap-build") {
            let r = bench(&mut || { let mut t = 0usize; let mut p = 0i64;
                for _ in 0..10 { let f: Arc<$map> = Arc::new(black_box(&m).clone()); t += f.len(); p = f[&k7]; } (t, p) },
                |&r| r == (10 * N, 7));
            row("immutablemap-build", $lib, $ver, (10 * N) as u64, r, "Arc::new(map.clone())");
        }
        if selected("immutablemap-lookup") {
            let f: Arc<$map> = Arc::new(m.clone());
            let r = bench(&mut || { let mut s = 0i64; for k in black_box($hit_q).iter() { s += f[k]; } s }, |&s| s == $hit_exp);
            row("immutablemap-lookup", $lib, $ver, Q as u64, r, "map behind Arc");
        }
        if selected("immutableset-build") {
            let r = bench(&mut || { let mut t = 0usize; let mut p = false;
                for _ in 0..10 { let f: Arc<$set> = Arc::new(black_box(&st).clone()); t += f.len(); p = f.contains(&k7); } (t, p) },
                |&r| r == (10 * N, true));
            row("immutableset-build", $libset, $ver, (10 * N) as u64, r, "Arc::new(set.clone())");
        }
        if selected("immutableset-contains") {
            let f: Arc<$set> = Arc::new(st.clone());
            let r = bench(&mut || { let mut c = 0i64; for k in black_box($half_q).iter() { if f.contains(k) { c += 1; } } c }, |&c| c == $half_exp);
            row("immutableset-contains", $libset, $ver, Q as u64, r, "set behind Arc");
        }
    }};
}

fn main() {
    let keys: Vec<i64> = (0..N as i64).map(key).collect();
    let hit_idx = Gen::new().indices(Q, N as i32);
    let hit_exp: i64 = hit_idx.iter().map(|&x| x as i64).sum();
    let hit_q: Vec<i64> = hit_idx.iter().map(|&i| keys[i as usize]).collect();
    let miss_q: Vec<i64> = hit_idx.iter().map(|&i| key(N as i64 + i as i64)).collect();
    let half_idx = Gen::new().indices(Q, 2 * N as i32);
    let half_exp = half_idx.iter().filter(|&&i| (i as usize) < N).count() as i64;
    let half_q: Vec<i64> = half_idx.iter().map(|&i| key(i as i64)).collect();
    drop(hit_idx);
    drop(half_idx);

    hash_rows!("std::collections::HashMap", "std::collections::HashSet", "std", std::collections::HashMap<i64, i64>, std::collections::HashSet<i64>,
        &keys, &hit_q, hit_exp, &miss_q, &half_q, half_exp);
    hash_rows!("hashbrown::HashMap", "hashbrown::HashSet", "0.17.1", hashbrown::HashMap<i64, i64>, hashbrown::HashSet<i64>,
        &keys, &hit_q, hit_exp, &miss_q, &half_q, half_exp);
}
