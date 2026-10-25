// Rust collection/ competitors, sequence half: the ArrayList, LinkedList, Heap,
// ImmutableList, Collectors and binary-search ids, same sizes, inputs and
// checks as the Cajeta benchmarks (bench/src/main/cajeta/bench/collection/).
// std only: Vec, LinkedList, BinaryHeap<Reverse>, Arc<[i32]>, iterator
// collect, a HashMap fold, slice::binary_search.
#[path = "../../common/collgen.rs"]
mod collgen;
use collgen::*;
use std::cmp::Reverse;
use std::collections::{BinaryHeap, HashMap, LinkedList};
use std::hint::black_box;
use std::sync::Arc;

const N1M: usize = 1_000_000;

fn main() {
    let lst1m: Vec<i32> = (0..N1M as i32).collect();
    let idx1m = Gen::new().indices(N1M, N1M as i32);
    let exp_get: i64 = idx1m.iter().map(|&x| x as i64).sum::<i64>() * 100;

    if selected("arraylist-get") {
        let m = bench(&mut || {
            let (l, t) = (black_box(&lst1m), black_box(&idx1m));
            let mut s = 0i64;
            for _ in 0..100 { for &j in t.iter() { s += l[j as usize] as i64; } }
            s
        }, |&s| s == exp_get);
        row("arraylist-get", "Vec", "std", 100_000_000, m, "indexed (bounds-checked)");
    }

    let lst1e7: Vec<i32> = (0..10_000_000).collect();
    if selected("arraylist-iterate") {
        let m = bench(&mut || {
            let l = black_box(&lst1e7);
            let mut s = 0i64;
            for _ in 0..10 { for &v in l.iter() { s += v as i64; } }
            s
        }, |&s| s == 10 * (10_000_000i64 * 9_999_999 / 2));
        row("arraylist-iterate", "Vec", "std", 100_000_000, m, "for v in vec.iter()");
    }

    if selected("arraylist-sort") {
        let mut g = Gen::new();
        let src: Vec<i32> = (0..N1M).map(|_| g.next()).collect();
        let m = bench(&mut || { let mut a = black_box(&src).clone(); a.sort_unstable(); a },
            |a| a.len() == N1M && a.windows(2).all(|w| w[0] <= w[1]));
        row("arraylist-sort", "Vec::sort_unstable", "std", N1M as u64, m, "includes the copy");
    }

    if selected("linkedlist-insert-traverse") {
        const N: i32 = 40000;
        let m = bench(&mut || {
            let mut l = LinkedList::new();
            for i in 0..N { l.push_back(i); }
            (l.len(), l.iter().any(|&v| v == N - 1))
        }, |&r| r == (N as usize, true));
        row("linkedlist-insert-traverse", "LinkedList", "std", N as u64, m, "");
    }

    if selected("heap-sort") {
        const N: usize = 50000;
        let m = bench(&mut || {
            let mut h = BinaryHeap::new();
            let mut x: i64 = 12345;
            for _ in 0..N { x = (x * 1103515245 + 12345) & 0x7FFFFFFF; h.push(Reverse(x as i32)); }
            let mut out = Vec::with_capacity(N);
            while let Some(Reverse(v)) = h.pop() { out.push(v); }
            out
        }, |a| a.len() == N && a.windows(2).all(|w| w[0] <= w[1]));
        row("heap-sort", "BinaryHeap", "std", N as u64, m, "Reverse for a min-heap");
    }

    if selected("heap-push-pop") {
        let mut g = Gen::new();
        let vals: Vec<i32> = (0..10_001_000).map(|_| g.next()).collect();
        let m = bench(&mut || {
            let v = black_box(&vals);
            let mut h: BinaryHeap<Reverse<i32>> = v[..1000].iter().map(|&x| Reverse(x)).collect();
            let mut c = 0i64;
            for &x in &v[1000..] { h.push(Reverse(x)); c += h.pop().unwrap().0 as i64; }
            c
        }, |&c| c == 10738394301026345);
        row("heap-push-pop", "BinaryHeap", "std", 10_000_000, m, "Reverse for a min-heap; push then pop");
    }

    if selected("immutablelist-build") {
        let m = bench(&mut || {
            let mut t = 0usize;
            let mut tail = 0;
            for _ in 0..100 { let a: Arc<[i32]> = Arc::from(&black_box(&lst1m)[..]); t += a.len(); tail = a[999_999]; }
            (t, tail)
        }, |&r| r == (100_000_000, 999_999));
        row("immutablelist-build", "Arc<[i32]>", "std", 100_000_000, m, "Arc::from(&vec[..])");
    }

    if selected("immutablelist-get") {
        let frozen: Arc<[i32]> = Arc::from(&lst1m[..]);
        let m = bench(&mut || {
            let (l, t) = (black_box(&frozen), black_box(&idx1m));
            let mut s = 0i64;
            for _ in 0..100 { for &j in t.iter() { s += l[j as usize] as i64; } }
            s
        }, |&s| s == exp_get);
        row("immutablelist-get", "Arc<[i32]>", "std", 100_000_000, m, "indexed (bounds-checked)");
    }

    if selected("collectors-to-list") {
        let m = bench(&mut || { let o: Vec<i32> = black_box(&lst1e7).iter().copied().collect(); o },
            |o| o.len() == 10_000_000 && o[9_999_999] == 9_999_999);
        row("collectors-to-list", "collect::<Vec>", "std", 10_000_000, m, "");
    }

    if selected("collectors-group-by") {
        let m = bench(&mut || {
            let g = black_box(&lst1e7).iter().fold(HashMap::<i32, Vec<i32>>::new(), |mut m, &x| {
                m.entry(x % 1000).or_default().push(x);
                m
            });
            (g.len(), g[&0].len())
        }, |&r| r == (1000, 10000));
        row("collectors-group-by", "HashMap fold", "std", 10_000_000, m, "entry().or_default().push");
    }

    if selected("binary-search") {
        const N: i64 = 50000;
        let m = bench(&mut || {
            let a: Vec<i64> = (0..N).map(|i| i * 2).collect();
            let mut hits = 0i64;
            let mut x: i64 = 12345;
            for _ in 0..N {
                x = (x * 1103515245 + 12345) & 0x7FFFFFFF;
                if a.binary_search(&((x % N) * 2)).is_ok() { hits += 1; }
            }
            hits
        }, |&h| h == N);
        row("binary-search", "slice::binary_search", "std", N as u64, m, "");
    }
}
