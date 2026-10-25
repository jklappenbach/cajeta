// concurrent/tasks-1m-footprint, Rust tokio (Tasks.md): park 1e6 tokio tasks on
// one tokio::sync::watch channel (each awaits changed() on its own receiver),
// read the resident set while all are parked, then drop the sender, which wakes
// every receiver, and await every JoinHandle. Own process, so peak_rss_kb (the
// row's memory metric) is this workload's. One trial: the row is memory; the
// time is spawn + park + release. Skips under 16 GB MemAvailable.
// CAJETA_BENCH_SMOKE=1 parks 1e4 tasks and says so in notes. A JoinError is
// propagated as a Result and makes the row invalid.
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};

fn env(k: &str, d: &str) -> String { std::env::var(k).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| d.to_string()) }

fn kb(path: &str, key: &str) -> i64 {
    std::fs::read_to_string(path).ok().and_then(|s| {
        s.lines().find(|l| l.starts_with(key)).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok())
    }).unwrap_or(-1)
}

async fn run(n: i64) -> Result<(i64, i64, i64, i64), String> {
    let (tx, rx) = tokio::sync::watch::channel(0u8);
    let started = Arc::new(AtomicI64::new(0));
    let done = Arc::new(AtomicI64::new(0));
    let before = kb("/proc/self/status", "VmRSS:");
    let mut hs = Vec::with_capacity(n as usize);
    for _ in 0..n {
        let (mut rx, started, done) = (rx.clone(), started.clone(), done.clone());
        hs.push(tokio::spawn(async move {
            started.fetch_add(1, Ordering::SeqCst);
            let _ = rx.changed().await; // Err once the sender is dropped
            done.fetch_add(1, Ordering::SeqCst);
        }));
    }
    while started.load(Ordering::SeqCst) < n { tokio::time::sleep(Duration::from_millis(1)).await; }
    tokio::time::sleep(Duration::from_millis(10)).await;
    let parked_rss = kb("/proc/self/status", "VmRSS:");
    let parked = started.load(Ordering::SeqCst);
    drop(tx);
    for h in hs { h.await.map_err(|e| e.to_string())?; }
    Ok((parked, done.load(Ordering::SeqCst), before, parked_rss))
}

fn main() {
    let smoke = matches!(env("CAJETA_BENCH_SMOKE", "").as_str(), "1" | "true");
    let n: i64 = if smoke { 10_000 } else { 1_000_000 };
    let head = format!("1,{},{},tasks-1m-footprint,concurrent,,{n},,{n},rust,{},tokio", env("PROFILE_RUN_ID", "local"),
        env("PROFILE_RUN_TS", ""), env("PROFILE_LANG_VERSION", ""));
    let avail = kb("/proc/meminfo", "MemAvailable:");
    if (0..16 * 1024 * 1024).contains(&avail) {
        println!("{head},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,MemAvailable {} MiB is under the 16 GB this row needs,", avail / 1024);
        return;
    }
    let rt = match tokio::runtime::Builder::new_multi_thread().enable_all().build() {
        Ok(rt) => rt,
        Err(e) => { println!("{head},,,0,0,-1,-1,-1,-1,,,-1,-1,-1,-1,-1,skipped,,tokio runtime failed: {e},"); return; }
    };
    let t0 = Instant::now();
    let r = rt.block_on(run(n));
    let ns = t0.elapsed().as_nanos() as i64;
    let (ok, grew, notes) = match r {
        Ok((parked, done, before, parked_rss)) => {
            let grew = parked_rss - before;
            (parked == n && done == n, grew,
             format!("one tokio::sync::watch channel; {n} tasks parked in changed(); RSS grew {grew} KiB while parked = {} B/task; one trial", grew * 1024 / n))
        }
        Err(e) => (false, -1, format!("join error: {e}")),
    };
    let notes = if smoke { format!("{notes}; smoke size: 1e4 tasks") } else { notes };
    println!("{head},1.53.1,-O3 lto target-cpu=native,0,1,{ns},{ns},{ns},{ns},{:.3},Mop/s,{},-1,-1,-1,{grew},{},{ok},{notes},",
        n as f64 / ns as f64 * 1e3, kb("/proc/self/status", "VmHWM:"), if ok { "ok" } else { "invalid" });
}
