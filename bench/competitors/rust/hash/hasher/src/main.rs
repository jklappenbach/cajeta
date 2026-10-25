// Rust hasher competitors (benchmark-reference unit 8), same inputs and folds
// as bench/src/main/cajeta/bench/hash/:
//   defaulthasher-string: 1e7 hashes cycling 1024 keys "k-" + (1e13 + k);
//   defaulthasher-int:    1e8 hashes of the i64 keys 0 .. 1e8-1;
//   hash-struct:          1e7 hashes cycling 4096 #[derive(Hash)] structs
//                         (id: i64 = k*7919, x: i32 = k, y: i32 = k ^ 0x5bd1).
// Each hash builds the hasher a HashMap builds per key: RandomState (one per
// run, as a map holds one) -> build_hasher() -> write -> finish. Fold:
// acc ^= h; low += h & 0xFFFF. The keys are random per process, so the check
// is a second pass under the same RandomState reproducing (acc, low), and
// neighbouring keys hashing apart.
use std::alloc::{GlobalAlloc, Layout, System};
use std::collections::hash_map::RandomState;
use std::hash::{BuildHasher, Hash, Hasher};
use std::hint::black_box;
use std::sync::atomic::{AtomicU64, Ordering as Ord_};
use std::time::Instant;

static ALLOCED: AtomicU64 = AtomicU64::new(0);
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { ALLOCED.fetch_add(l.size() as u64, Ord_::Relaxed); System.alloc(l) }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { System.dealloc(p, l) }
}
#[global_allocator]
static GA: Counting = Counting;

#[derive(Hash)]
struct Key3 { id: i64, x: i32, y: i32 }

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }
fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:")).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}
struct Cfg { run_id: String, ts: String, warmup: usize, trials: usize }

fn measure<F: FnMut() -> (u64, u64)>(c: &Cfg, bench: &str, n: u64, lib: &str, notes: &str, mut f: F, extra_ok: bool) {
    let first = f();
    for _ in 0..c.warmup { black_box(f()); }
    let mut s: Vec<u128> = Vec::with_capacity(c.trials);
    let mut ok = extra_ok && first.1 > 0;
    for _ in 0..c.trials {
        let t = Instant::now();
        let r = black_box(f());
        s.push(t.elapsed().as_nanos());
        ok &= r == first;
    }
    ALLOCED.store(0, Ord_::Relaxed);
    black_box(f());
    let alloc = ALLOCED.load(Ord_::Relaxed);
    s.sort_unstable();
    let k = s.len();
    let sum: u128 = s.iter().sum();
    let (mn, med, mean, p95) = (s[0], s[k / 2], sum / k as u128, s[(k * 95 / 100).min(k - 1)]);
    let mops = if med > 0 { n as f64 / med as f64 * 1e3 } else { 0.0 };
    println!("1,{},{},{},hash,,{},,{},rust,{},{},,-O3 lto target-cpu=native,{},{},{},{},{},{},{:.2},Mop/s,{},-1,{},-1,-1,{},{},{},",
        c.run_id, c.ts, bench, n, n, env("PROFILE_LANG_VERSION", ""), lib, c.warmup, c.trials,
        mn, med, mean, p95, mops, peak_rss_kb(), alloc, if ok { "ok" } else { "invalid" }, ok, notes);
}

fn main() {
    let c = Cfg {
        run_id: env("PROFILE_RUN_ID", "local"), ts: env("PROFILE_RUN_TS", ""),
        warmup: env("PROFILE_WARMUP", "3").parse().unwrap_or(3),
        trials: env("PROFILE_TRIALS", "10").parse().unwrap_or(10),
    };
    let rs = RandomState::new();

    let pool: Vec<String> = (0..1024u64).map(|k| format!("k-{}", 10_000_000_000_000u64 + k)).collect();
    let sok = pool.iter().all(|s| s.len() == 16) && rs.hash_one(&pool[0]) != rs.hash_one(&pool[1]);
    measure(&c, "defaulthasher-string", 10_000_000, "std", "RandomState DefaultHasher (SipHash-1-3) per key: str Hash", || {
        let (mut a, mut l) = (0u64, 0u64);
        for i in 0..10_000_000usize {
            let mut h = rs.build_hasher();
            black_box(&pool[i & 1023]).hash(&mut h);
            let v = h.finish();
            a ^= v; l += v & 0xFFFF;
        }
        (a, l)
    }, sok);

    let iok = rs.hash_one(0i64) != rs.hash_one(1i64);
    measure(&c, "defaulthasher-int", 100_000_000, "std", "RandomState DefaultHasher (SipHash-1-3) per key: write_i64", || {
        let (mut a, mut l) = (0u64, 0u64);
        for i in 0..100_000_000i64 {
            let mut h = rs.build_hasher();
            h.write_i64(black_box(i));
            let v = h.finish();
            a ^= v; l += v & 0xFFFF;
        }
        (a, l)
    }, iok);

    let keys: Vec<Key3> = (0..4096i64).map(|k| Key3 { id: k * 7919, x: k as i32, y: (k as i32) ^ 0x5bd1 }).collect();
    let kok = keys.windows(2).all(|w| rs.hash_one(&w[0]) != rs.hash_one(&w[1]))
        && rs.hash_one(&keys[7]) == rs.hash_one(&Key3 { id: 7 * 7919, x: 7, y: 7 ^ 0x5bd1 });
    measure(&c, "hash-struct", 10_000_000, "std", "#[derive(Hash)] through RandomState DefaultHasher (SipHash-1-3)", || {
        let (mut a, mut l) = (0u64, 0u64);
        for i in 0..10_000_000usize {
            let v = rs.hash_one(black_box(&keys[i & 4095]));
            a ^= v; l += v & 0xFFFF;
        }
        (a, l)
    }, kok);
}
