// Rust concurrency competitors for bench/concurrent/*.md — every id, the same
// workload, size and check as the Cajeta side. One v1 row per id (lib/rows.sh
// lifts it to v2). Contended rows use OS threads (std::thread::scope) where
// Cajeta uses fibers; the tokio rows (Semaphore, sleep, spawn) run on a tokio
// runtime. Under CAJETA_BENCH_SMOKE=1 the multi-thread rows run 1e4 operations
// per thread (channels: 1e4 messages), the 1e8 single-thread rows 1e6 and the
// sleep row 100 sleeps; those rows say "smoke size" in notes. A failure inside
// a workload (a poisoned lock, a JoinError) is propagated as a Result and turns
// the row's check false; nothing panics silently. Results are consumed
// (returned, checked and passed through black_box).
use std::alloc::{GlobalAlloc, Layout, System};
use std::cell::Cell;
use std::hint::black_box;
use std::sync::atomic::{compiler_fence, AtomicI32, AtomicI64, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, Instant};

static ALLOCED: AtomicU64 = AtomicU64::new(0);
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { ALLOCED.fetch_add(l.size() as u64, Ordering::Relaxed); System.alloc(l) }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { System.dealloc(p, l) }
}
#[global_allocator]
static GA: Counting = Counting;

type R = Result<i64, String>;

fn env(k: &str, d: &str) -> String { std::env::var(k).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| d.to_string()) }

fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok().and_then(|s| {
        s.lines().find(|l| l.starts_with("VmHWM:")).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

struct Ctx { run_id: String, ts: String, warmup: usize, trials: usize, smoke: bool, ver: String }

impl Ctx {
    fn smoke_note(&self, what: &str) -> String { if self.smoke { format!("smoke size: {what}") } else { String::new() } }

    #[allow(clippy::too_many_arguments)]
    fn emit(&self, bench: &str, lib: &str, libver: &str, input: i64, warmup: usize, mut s: Vec<u128>, ok: bool, alloc: u64, notes: &str) {
        s.sort_unstable();
        let n = s.len();
        let sum: u128 = s.iter().sum();
        let (mn, med, mean, p95) = (s[0], s[n / 2], sum / n as u128, s[(n * 95 / 100).min(n - 1)]);
        let mops = if med > 0 { input as f64 / med as f64 * 1e9 / 1e6 } else { 0.0 };
        let status = if ok { "ok" } else { "invalid" };
        println!("1,{},{},{bench},concurrent,,{input},,{input},rust,{},{lib},{libver},-O3 lto target-cpu=native,{warmup},{n},\
{mn},{med},{mean},{p95},{mops:.3},Mop/s,{},-1,{alloc},-1,-1,{status},{ok},{notes},",
            self.run_id, self.ts, self.ver, peak_rss_kb());
    }

    fn skip(&self, bench: &str, lib: &str, input: i64, reason: &str) {
        println!("1,{},{},{bench},concurrent,,{input},,{input},rust,{},{lib},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,{reason},",
            self.run_id, self.ts, self.ver);
    }

    fn bench<F: FnMut() -> R>(&self, bench: &str, lib: &str, libver: &str, input: i64, notes: &str, mut f: F, want: i64) {
        for _ in 0..self.warmup { let _ = black_box(f()); }
        let mut s = Vec::with_capacity(self.trials);
        let mut ok = true;
        for _ in 0..self.trials {
            let t = Instant::now();
            let r = f();
            s.push(t.elapsed().as_nanos());
            ok &= matches!(r, Ok(v) if v == want);
            black_box(r).ok();
        }
        ALLOCED.store(0, Ordering::Relaxed);
        let _ = black_box(f());
        let al = ALLOCED.load(Ordering::Relaxed);
        self.emit(bench, lib, libver, input, self.warmup, s, ok, al, notes);
    }
}

fn join(a: &str, b: &str) -> String { if b.is_empty() { a.to_string() } else { format!("{a}; {b}") } }

thread_local! { static TL: Cell<i64> = const { Cell::new(0) }; }

fn threads<F: Fn(usize) -> R + Sync>(t: usize, f: F) -> Result<Vec<i64>, String> {
    std::thread::scope(|sc| {
        let hs: Vec<_> = (0..t).map(|k| { let f = &f; sc.spawn(move || f(k)) }).collect();
        hs.into_iter().map(|h| h.join().map_err(|_| "thread panicked".to_string())?).collect()
    })
}

fn main() {
    let smoke = matches!(env("CAJETA_BENCH_SMOKE", "").as_str(), "1" | "true");
    let c = Ctx {
        run_id: env("PROFILE_RUN_ID", "local"), ts: env("PROFILE_RUN_TS", ""),
        warmup: env("PROFILE_WARMUP", "3").parse().unwrap_or(3), trials: env("PROFILE_TRIALS", "10").parse().unwrap_or(10),
        smoke, ver: env("PROFILE_LANG_VERSION", ""),
    };
    let n1: i64 = if smoke { 1_000_000 } else { 100_000_000 };
    let per: i64 = if smoke { 10_000 } else { 1_000_000 };
    let nch: i64 = if smoke { 10_000 } else { 5_000_000 };
    let nsp: i64 = if smoke { 10_000 } else { 100_000 };
    let nsleep: usize = if smoke { 100 } else { 10_000 };
    let s1 = c.smoke_note("1e6 ops");
    let sc = c.smoke_note("1e4 ops per thread");

    c.bench("atomic-fetchadd", "AtomicI64", "std", n1, &join("SeqCst fetch_add", &s1), || {
        let a = AtomicI64::new(0);
        for _ in 0..n1 { a.fetch_add(1, Ordering::SeqCst); }
        Ok(a.load(Ordering::SeqCst))
    }, n1);
    c.bench("atomic-fetchadd-32", "AtomicI32", "std", n1, &join("SeqCst fetch_add", &s1), || {
        let a = AtomicI32::new(0);
        for _ in 0..n1 { a.fetch_add(1, Ordering::SeqCst); }
        Ok(a.load(Ordering::SeqCst) as i64)
    }, n1);
    c.bench("atomic-cas", "AtomicI64", "std", n1, &join("compare_exchange SeqCst; uncontended", &s1), || {
        let a = AtomicI64::new(0);
        for i in 0..n1 { a.compare_exchange(i, i + 1, Ordering::SeqCst, Ordering::SeqCst).map_err(|v| format!("cas failed at {v}"))?; }
        Ok(a.load(Ordering::SeqCst))
    }, n1);
    c.bench("atomic-contended-16", "AtomicI64+std::thread", "std", 16 * per, &join("16 OS threads", &sc), || {
        let a = AtomicI64::new(0);
        threads(16, |_| { for _ in 0..per { a.fetch_add(1, Ordering::SeqCst); } Ok(0) })?;
        Ok(a.load(Ordering::SeqCst))
    }, 16 * per);
    c.bench("mutex-uncontended", "std::sync::Mutex", "std", n1, &join("lock; += 1; unlock on one thread", &s1), || {
        let m = Mutex::new(0i64);
        for _ in 0..n1 { *m.lock().map_err(|e| e.to_string())? += 1; }
        m.into_inner().map_err(|e| e.to_string())
    }, n1);
    for t in [4i64, 16] {
        c.bench(&format!("mutex-contended-{t}"), "std::sync::Mutex+std::thread", "std", t * per, &join(&format!("{t} OS threads"), &sc), || {
            let m = Mutex::new(0i64);
            threads(t as usize, |_| { for _ in 0..per { *m.lock().map_err(|e| e.to_string())? += 1; } Ok(0) })?;
            m.into_inner().map_err(|e| e.to_string())
        }, t * per);
    }
    c.bench("rwlock-read-heavy", "std::sync::RwLock", "std", 16 * per, &join("16 OS threads 90/10 read/write", &sc), || {
        let m = RwLock::new(0i64);
        let reads = threads(16, |_| {
            let mut s = 0i64;
            for i in 0..per {
                if i % 10 == 0 { *m.write().map_err(|e| e.to_string())? += 1; } else { s += *m.read().map_err(|e| e.to_string())?; }
            }
            Ok(s)
        })?;
        black_box(reads);
        m.into_inner().map_err(|e| e.to_string())
    }, 16 * (per / 10));
    c.bench("lock-uncontended", "std::sync::Mutex<()>", "std", n1, &join("guard per pass on one thread", &s1), || {
        let m = Mutex::new(());
        let mut count = 0i64;
        for _ in 0..n1 { let _g = m.lock().map_err(|e| e.to_string())?; count += 1; }
        Ok(count)
    }, n1);

    // tokio Semaphore: 16 tasks on a 16-worker multi-thread runtime.
    let rt16 = tokio::runtime::Builder::new_multi_thread().worker_threads(16).enable_all().build().expect("tokio runtime");
    c.bench("semaphore-acquire-release", "tokio::sync::Semaphore", "1.53.1", 16 * per,
        &join("16 tokio tasks on 16 worker threads through 4 permits", &c.smoke_note("1e4 passes per task")), || {
        rt16.block_on(async {
            let sem = Arc::new(tokio::sync::Semaphore::new(4));
            let inside = Arc::new(AtomicI64::new(0));
            let viol = Arc::new(AtomicI64::new(0));
            let mut hs = Vec::new();
            for _ in 0..16 {
                let (sem, inside, viol) = (sem.clone(), inside.clone(), viol.clone());
                hs.push(tokio::spawn(async move {
                    for _ in 0..per {
                        let _p = sem.acquire().await.map_err(|e| e.to_string())?;
                        if inside.fetch_add(1, Ordering::SeqCst) + 1 > 4 { viol.fetch_add(1, Ordering::SeqCst); }
                        inside.fetch_sub(1, Ordering::SeqCst);
                    }
                    Ok::<i64, String>(per)
                }));
            }
            let mut passes = 0i64;
            for h in hs { passes += h.await.map_err(|e| e.to_string())??; }
            if viol.load(Ordering::SeqCst) != 0 { return Err("more than 4 holders".to_string()); }
            Ok(passes)
        })
    }, 16 * per);

    let want_ch = nch * (nch + 1) / 2;
    let chn = c.smoke_note("1e4 messages");
    for (id, p, q) in [("channel-spsc", 1usize, 1usize), ("channel-mpsc", 4, 1), ("channel-mpmc", 4, 4)] {
        c.bench(id, "crossbeam-channel", "0.5.16", nch, &join("bounded(1000); OS threads", &chn), || {
            let (tx, rx) = crossbeam_channel::bounded::<i64>(1000);
            let pp = nch / p as i64;
            let r = std::thread::scope(|sc| {
                let cons: Vec<_> = (0..q).map(|_| { let rx = rx.clone(); sc.spawn(move || { let (mut s, mut k) = (0i64, 0i64); for v in rx.iter() { s += v; k += 1; } (s, k) }) }).collect();
                let prods: Vec<_> = (0..p).map(|pi| { let tx = tx.clone(); sc.spawn(move || -> Result<(), String> {
                    for i in 0..pp { tx.send(pi as i64 * pp + i + 1).map_err(|e| e.to_string())?; } Ok(()) }) }).collect();
                drop(tx);
                for h in prods { h.join().map_err(|_| "producer panicked".to_string())??; }
                let mut s = 0i64; let mut k = 0i64;
                for h in cons { let (a, b) = h.join().map_err(|_| "consumer panicked".to_string())?; s += a; k += b; }
                if k != nch { return Err(format!("received {k}")); }
                Ok(s)
            });
            r
        }, want_ch);
        if q == 1 {
            c.bench(id, "std::sync::mpsc::sync_channel", "std", nch, &join("sync_channel(1000); OS threads", &chn), || {
                let (tx, rx) = std::sync::mpsc::sync_channel::<i64>(1000);
                let pp = nch / p as i64;
                std::thread::scope(|sc| {
                    let cons = sc.spawn(move || { let (mut s, mut k) = (0i64, 0i64); for v in rx.iter() { s += v; k += 1; } (s, k) });
                    let prods: Vec<_> = (0..p).map(|pi| { let tx = tx.clone(); sc.spawn(move || -> Result<(), String> {
                        for i in 0..pp { tx.send(pi as i64 * pp + i + 1).map_err(|e| e.to_string())?; } Ok(()) }) }).collect();
                    drop(tx);
                    for h in prods { h.join().map_err(|_| "producer panicked".to_string())??; }
                    let (s, k) = cons.join().map_err(|_| "consumer panicked".to_string())?;
                    if k != nch { return Err(format!("received {k}")); }
                    Ok(s)
                })
            }, want_ch);
        } else {
            c.skip(id, "std::sync::mpsc::sync_channel", nch, "std::sync::mpsc has a single receiver; no mpmc shape (crossbeam's harness skips it too)");
        }
    }

    // fiber-sleep-wake: one sample per sleep, the overshoot past 1 ms.
    {
        let rt1 = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio runtime");
        let warm = 10usize;
        let samples: Vec<u128> = rt1.block_on(async {
            let mut v = Vec::with_capacity(nsleep);
            for k in 0..(warm + nsleep) {
                let t = Instant::now();
                tokio::time::sleep(Duration::from_millis(1)).await;
                let over = t.elapsed().saturating_sub(Duration::from_millis(1)).as_nanos();
                if k >= warm { v.push(over); }
            }
            v
        });
        let ok = samples.len() == nsleep;
        c.emit("fiber-sleep-wake", "tokio::time::sleep", "1.53.1", 1, warm, samples, ok, 0,
            &join("row = overshoot past 1 ms per sleep over the trials; current_thread runtime; tokio's timer wheel has 1 ms resolution",
                  &c.smoke_note("100 sleeps")));
    }

    c.bench("fiberlocal-get-set", "thread_local! Cell<i64>", "std", n1,
        &join("set(i) then sum += get() per pass with a compiler fence so both happen", &s1), || {
        let mut sum = 0i64;
        for i in 0..n1 {
            TL.with(|t| t.set(i));
            compiler_fence(Ordering::SeqCst);
            sum += TL.with(|t| t.get());
        }
        Ok(sum)
    }, n1 * (n1 - 1) / 2);

    let rtm = tokio::runtime::Builder::new_multi_thread().enable_all().build().expect("tokio runtime");
    c.bench("task-spawn-await", "tokio::spawn", "1.53.1", nsp, &join("spawn then await each from a task on the multi-thread runtime", &c.smoke_note("1e4 tasks")), || {
        // The loop itself runs as a task, so spawn and await happen on a worker.
        rtm.block_on(async {
            tokio::spawn(async move {
                let mut sum = 0i64;
                for i in 0..nsp { sum += tokio::spawn(async move { i + 1 }).await.map_err(|e| e.to_string())?; }
                Ok::<i64, String>(sum)
            }).await.map_err(|e| e.to_string())?
        })
    }, nsp * (nsp + 1) / 2);
}
