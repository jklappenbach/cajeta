// process/ competitor in Rust (bench/process/Command.md, Process.md): std::process::Command.
//   command-spawn-wait   Command::new("/bin/true").status() R = 1e3 times (smoke 1e2); check: every exit 0
//   process-pipe-100mb   100 MiB (smoke 10 MiB) of data[i] = (i*31+7) & 0xFF through `cat` with piped stdio:
//                        a writer thread owns the child's stdin, the main thread read_to_end()s stdout;
//                        check: length and byte sum equal the input's.
// Errors propagate as io::Result: a failed trial makes the row invalid with the error in notes.
// Rows are v1 (31 columns); competitors/lib/rows.sh lifts them to v2.
use std::io::{self, Read, Write};
use std::process::{Command, Stdio};
use std::time::Instant;

fn env(k: &str, d: &str) -> String { std::env::var(k).ok().filter(|v| !v.is_empty()).unwrap_or_else(|| d.to_string()) }
fn env_n(k: &str, d: usize) -> usize { env(k, "").parse().unwrap_or(d) }
fn smoke() -> bool { matches!(env("CAJETA_BENCH_SMOKE", "").as_str(), "1" | "true" | "yes") }
fn peak_rss_kb() -> i64 {
    std::fs::read_to_string("/proc/self/status").ok()
        .and_then(|s| s.lines().find(|l| l.starts_with("VmHWM:")).and_then(|l| l.split_whitespace().nth(1)).and_then(|v| v.parse().ok()))
        .unwrap_or(-1)
}

fn measure<F: FnMut() -> io::Result<()>>(warmup: usize, trials: usize, mut f: F) -> io::Result<[u128; 4]> {
    for _ in 0..warmup { f()?; }
    let mut v = Vec::new();
    for _ in 0..trials.max(1) { let t = Instant::now(); f()?; v.push(t.elapsed().as_nanos()); }
    v.sort_unstable();
    let n = v.len();
    Ok([v[0], v[n / 2], v.iter().sum::<u128>() / n as u128, v[(n * 95 / 100).min(n - 1)]])
}

fn emit(slug: &str, input: u64, w: usize, t: usize, st: io::Result<[u128; 4]>, ok: bool, notes: &str, mbps: bool) {
    let (s, ok, notes) = match st {
        Ok(s) => (s.map(|x| x as i128), ok, notes.to_string()),
        Err(e) => ([-1; 4], false, format!("{}; error: {}", notes, e)),
    };
    let (thr, unit) = if mbps && s[1] > 0 { (format!("{:.1}", input as f64 / s[1] as f64 * 1e9 / 1048576.0), "MB/s") } else { (String::new(), "") };
    println!("1,{},{},{},process,,0,,{},rust,{},std::process,{},-O3 lto target-cpu=native,{},{},{},{},{},{},{},{},{},-1,-1,-1,-1,{},{},{},",
        env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, input, env("PROFILE_LANG_VERSION", ""),
        env("PROFILE_LANG_VERSION", "").replace("rustc ", "").split(' ').next().unwrap_or(""), w, t, s[0], s[1], s[2], s[3],
        thr, unit, peak_rss_kb(), if ok { "ok" } else { "invalid" }, ok, notes.replace(',', ";"));
}

fn main() {
    let warmup = env_n("PROFILE_WARMUP", 1);
    let trials = env_n("PROFILE_TRIALS", 5).max(1);
    let sm = smoke();

    // ---- command-spawn-wait ----
    let r = if sm { 100 } else { 1000 };
    let mut zero = 0u64;
    let st = measure(warmup, trials, || {
        let mut z = 0u64;
        for _ in 0..r {
            if Command::new("/bin/true").status()?.success() { z += 1; }
        }
        zero = std::hint::black_box(z);
        Ok(())
    });
    let mut n1 = format!("Command::new(/bin/true).status() x{} per trial; errors propagate as Result; check: zero exits == {}", r, r);
    if sm { n1 += "; smoke size 1e2 spawns in place of 1e3"; }
    emit("command-spawn-wait", r, warmup, trials, st, zero == r, &n1, false);

    // ---- process-pipe-100mb ----
    let n: usize = if sm { 10 << 20 } else { 100 << 20 };
    let data: std::sync::Arc<Vec<u8>> = std::sync::Arc::new((0..n).map(|i| ((i * 31 + 7) & 0xFF) as u8).collect());
    let want: u64 = data.iter().map(|&b| b as u64).sum();
    let (mut got_len, mut got_sum) = (0usize, 0u64);
    let sp = measure(warmup, trials, || {
        let mut child = Command::new("cat").stdin(Stdio::piped()).stdout(Stdio::piped()).spawn()?;
        let mut stdin = child.stdin.take().ok_or_else(|| io::Error::new(io::ErrorKind::Other, "no stdin"))?;
        let d = data.clone();
        let wr = std::thread::spawn(move || -> io::Result<()> { stdin.write_all(&d)?; Ok(()) });
        let mut out = Vec::with_capacity(n);
        child.stdout.take().ok_or_else(|| io::Error::new(io::ErrorKind::Other, "no stdout"))?.read_to_end(&mut out)?;
        wr.join().map_err(|_| io::Error::new(io::ErrorKind::Other, "writer panicked"))??;
        child.wait()?;
        got_len = out.len();
        got_sum = std::hint::black_box(out.iter().map(|&b| b as u64).sum());
        Ok(())
    });
    let mut n2 = format!("Command::new(cat) piped; writer thread write_all + read_to_end; errors propagate as Result; check: length {} and byte sum {}", n, want);
    if sm { n2 += "; smoke size 10 MiB in place of 100 MiB"; }
    emit("process-pipe-100mb", n as u64, warmup, trials, sp, got_len == n && got_sum == want, &n2, true);
}
