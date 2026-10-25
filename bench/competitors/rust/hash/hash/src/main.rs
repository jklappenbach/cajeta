// Rust hash competitors (benchmark-reference unit 8): the hash/ ids of
// bench/hash/XXHash3.md and SipHash.md, same inputs and checks as
// bench/src/main/cajeta/bench/hash/:
//   xxhash3 / xxhash3_128 / siphash: 1e7 hashes of a 32-byte key whose first 8
//     bytes are i little-endian (byte j = (j*31+7) & 0xFF);
//   xxhash3-256k / xxhash3_128-256k: 1e4 hashes of a 256 KB buffer, same rule.
// Every digest is XORed into the checksum, checked against the reference
// constants (xxHash 0.8.3 at seed 0; the SipHash-2-4 reference C under key
// 00..0f). Libraries: xxhash-rust, siphasher.
use siphasher::sip::SipHasher24;
use std::alloc::{GlobalAlloc, Layout, System};
use std::hint::black_box;
use std::sync::atomic::{AtomicU64, Ordering as Ord_};
use std::time::Instant;
use xxhash_rust::xxh3::{xxh3_128, xxh3_64};

static ALLOCED: AtomicU64 = AtomicU64::new(0);
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { ALLOCED.fetch_add(l.size() as u64, Ord_::Relaxed); System.alloc(l) }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { System.dealloc(p, l) }
}
#[global_allocator]
static GA: Counting = Counting;

const XXH3_REF: u64 = 0x15ae4a024a0e82de;
const XXH3_128_REF: (u64, u64) = (0x7f4adb2d9f387c7f, 0xb318fb3ec50537ef);
const XXH3_256K_REF: u64 = 0x4f52c915522f9fe0;
const XXH3_128_256K_REF: (u64, u64) = (0x4f52c915522f9fe0, 0x3c843c6da63e2dc3);
const SIP_REF: u64 = 0x1a6999cca3276432;
const N_KEYS: u64 = 10_000_000;
const N_BIG: u64 = 10_000;
const BIG: usize = 262_144;

fn env(k: &str, d: &str) -> String { std::env::var(k).unwrap_or_else(|_| d.to_string()) }
fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:")).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}
fn buf(n: usize) -> Vec<u8> { (0..n).map(|j| ((j * 31 + 7) & 0xFF) as u8).collect() }

struct Cfg { run_id: String, ts: String, warmup: usize, trials: usize }

fn measure<F: FnMut() -> (u64, u64)>(c: &Cfg, bench: &str, bytes: u64, lib: &str, ver: &str, notes: &str,
                                     mut f: F, want: (u64, u64)) {
    for _ in 0..c.warmup { black_box(f()); }
    let mut s: Vec<u128> = Vec::with_capacity(c.trials);
    let mut ok = true;
    for _ in 0..c.trials {
        let t = Instant::now();
        let r = black_box(f());
        s.push(t.elapsed().as_nanos());
        ok &= r == want;
    }
    ALLOCED.store(0, Ord_::Relaxed);
    black_box(f());
    let alloc = ALLOCED.load(Ord_::Relaxed);
    s.sort_unstable();
    let k = s.len();
    let sum: u128 = s.iter().sum();
    let (mn, med, mean, p95) = (s[0], s[k / 2], sum / k as u128, s[(k * 95 / 100).min(k - 1)]);
    let mbps = if med > 0 { bytes as f64 / med as f64 * 1e9 / 1_048_576.0 } else { 0.0 };
    println!("1,{},{},{},hash,,{},,{},rust,{},{},{},-O3 lto target-cpu=native,{},{},{},{},{},{},{:.1},MB/s,{},-1,{},-1,-1,{},{},{},",
        c.run_id, c.ts, bench, bytes, bytes, env("PROFILE_LANG_VERSION", ""), lib, ver, c.warmup, c.trials,
        mn, med, mean, p95, mbps, peak_rss_kb(), alloc, if ok { "ok" } else { "invalid" }, ok, notes);
}

fn main() {
    let c = Cfg {
        run_id: env("PROFILE_RUN_ID", "local"), ts: env("PROFILE_RUN_TS", ""),
        warmup: env("PROFILE_WARMUP", "3").parse().unwrap_or(3),
        trials: env("PROFILE_TRIALS", "10").parse().unwrap_or(10),
    };
    let mut key = buf(32);
    let mut big = buf(BIG);

    measure(&c, "xxhash3", N_KEYS * 32, "xxhash-rust", "0.8.19", "xxh3_64", || {
        let mut a = 0u64;
        for i in 0..N_KEYS { key[..8].copy_from_slice(&i.to_le_bytes()); a ^= xxh3_64(black_box(&key)); }
        (a, 0)
    }, (XXH3_REF, 0));
    measure(&c, "xxhash3_128", N_KEYS * 32, "xxhash-rust", "0.8.19", "xxh3_128", || {
        let (mut lo, mut hi) = (0u64, 0u64);
        for i in 0..N_KEYS {
            key[..8].copy_from_slice(&i.to_le_bytes());
            let h = xxh3_128(black_box(&key));
            lo ^= h as u64; hi ^= (h >> 64) as u64;
        }
        (lo, hi)
    }, XXH3_128_REF);
    measure(&c, "xxhash3-256k", N_BIG * BIG as u64, "xxhash-rust", "0.8.19", "xxh3_64", || {
        let mut a = 0u64;
        for i in 0..N_BIG { big[..8].copy_from_slice(&i.to_le_bytes()); a ^= xxh3_64(black_box(&big)); }
        (a, 0)
    }, (XXH3_256K_REF, 0));
    measure(&c, "xxhash3_128-256k", N_BIG * BIG as u64, "xxhash-rust", "0.8.19", "xxh3_128", || {
        let (mut lo, mut hi) = (0u64, 0u64);
        for i in 0..N_BIG {
            big[..8].copy_from_slice(&i.to_le_bytes());
            let h = xxh3_128(black_box(&big));
            lo ^= h as u64; hi ^= (h >> 64) as u64;
        }
        (lo, hi)
    }, XXH3_128_256K_REF);
    let k16: [u8; 16] = core::array::from_fn(|j| j as u8);
    let sip = SipHasher24::new_with_key(&k16);
    let vec_ok = sip.hash(&(0u8..15).collect::<Vec<u8>>()) == 0xa129ca6149be45e5;
    measure(&c, "siphash", N_KEYS * 32, "siphasher", "1.0.4", "SipHasher24::hash (key 00..0f)", || {
        let mut a = 0u64;
        for i in 0..N_KEYS { key[..8].copy_from_slice(&i.to_le_bytes()); a ^= sip.hash(black_box(&key)); }
        (if vec_ok { a } else { 0 }, 0)
    }, (SIP_REF, 0));
}
