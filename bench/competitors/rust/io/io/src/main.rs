// io/ competitor, Rust std (bench/io/*.md): Vec<u8> and slices, std::fs,
// BufRead::lines, BufWriter, PathBuf, std::net with the peer on a thread,
// walkdir, notify, bytes and url. Sizes, inputs, ports and checks are those of
// competitors/drivers/io/io.sh; every row's notes carry check=<value>. I/O
// errors propagate as Result: a failing workload is an invalid row with the
// error in its notes, never a panic. Usage: io-std [id ...].
mod common;
use bytes::{Buf, BufMut, BytesMut};
use common::*;
use std::fs::{self, File};
use std::io::{self, BufRead, BufReader, BufWriter, Read, Write};
use std::net::{Shutdown, SocketAddr, TcpListener, TcpStream, UdpSocket};
use std::os::unix::fs::FileExt;
use std::path::{Component, Path, PathBuf};
use std::sync::mpsc;
use std::thread;
use std::time::{Duration, Instant};

const PORT_BASE: u16 = 61300;
const NOTE_ERR: &str = "errors propagate as Result";

struct Ctx { only: Vec<String>, dir: String, scale: &'static str, dev: String, ver: String }
impl Ctx {
    fn p(&self, n: &str) -> String { format!("{}/{}", self.dir, n) }
    fn want(&self, id: &str) -> bool { want(&self.only, id) }
}
fn seq_bytes() -> u64 { pick(1073741824, 67108864) }
fn line_count() -> u64 { pick(10_000_000, 1_000_000) }
fn pid_note(slot: u16) -> String { format!("; port {}; pid {}", PORT_BASE + slot, std::process::id()) }

fn report<F: FnOnce() -> io::Result<()>>(slug: &str, lib: &str, ver: &str, f: F) {
    if let Err(e) = f() { emit_err(slug, lib, ver, &e); }
}

// ------------------------------------------------------------ buffer
fn buffer_rows(c: &Ctx) {
    const N: usize = 16_777_216;
    if c.want("buffer-fill") {
        let passes = pick(100, 4) as usize;
        let mut exp: u64 = 0;
        for p in 0..passes { exp += (((p * 7919) % N + p) & 255) as u64; }
        exp += (N as u64 / 256) * 32640;
        let mut acc = 0u64;
        let (w, t) = (warm(2), trials(10));
        let s = measure::<(), _>(w, t, || {
            let mut d = vec![0u8; N];
            let mut a = 0u64;
            for p in 0..passes {
                for (i, b) in d.iter_mut().enumerate() { *b = ((i + p) & 255) as u8; }
                a += d[(p * 7919) % N] as u64;
            }
            a += d.iter().map(|&b| b as u64).sum::<u64>();
            acc = std::hint::black_box(a);
            Ok(())
        }).unwrap();
        emit("buffer-fill", (N * passes / 1048576) as i64, "Vec<u8>", &c.ver, w, t, &s, acc == exp,
             &format!("check={acc}; 16 MiB x {passes} passes{}", sm("100 passes")), "MB/s");
    }
    if c.want("buffer-read-primitives") {
        let reads = pick(50_000_000, 5_000_000) as usize;
        let mut d = vec![0u8; N + 8];
        for k in 0..2_097_152usize { d[k * 4..k * 4 + 4].copy_from_slice(&((k as u32).wrapping_mul(2654435761)).to_le_bytes()); }
        for k in 0..1_048_576usize { d[8_388_608 + k * 8..8_388_608 + k * 8 + 8].copy_from_slice(&(((k & 1023) as f64) * 0.5).to_le_bytes()); }
        let (mut ei, mut ef) = (0i64, 0f64);
        for j in 0..reads { ei += ((j & 2_097_151) as u32).wrapping_mul(2654435761) as i32 as i64; ef += ((j & 1_048_575) & 1023) as f64 * 0.5; }
        let (mut si, mut sf) = (0i64, 0f64);
        let (w, t) = (warm(2), trials(10));
        let s = measure::<(), _>(w, t, || {
            let (mut a, mut b) = (0i64, 0f64);
            let dd = &d[..];
            for j in 0..reads {
                let oi = (j & 2_097_151) * 4;
                let of = 8_388_608 + (j & 1_048_575) * 8;
                a += i32::from_le_bytes(dd[oi..oi + 4].try_into().unwrap()) as i64;
                b += f64::from_le_bytes(dd[of..of + 8].try_into().unwrap());
            }
            si = std::hint::black_box(a); sf = std::hint::black_box(b);
            Ok(())
        }).unwrap();
        emit("buffer-read-primitives", (reads * 2) as i64, "slice from_le_bytes", &c.ver, w, t, &s, si == ei && sf == ef,
             &format!("check={si}/{}; {} values{}", (sf * 2.0) as i64, reads * 2, sm("1e8")), "values/s");
    }
    if c.want("buffer-slice") {
        let views = pick(10_000_000, 1_000_000) as usize;
        let span = N - 64;
        let d: Vec<u8> = (0..N).map(|i| ((i * 31 + 7) & 255) as u8).collect();
        let exp: u64 = (0..views).map(|j| ((((j * 4099) % span) * 31 + 7) & 255) as u64).sum();
        let mut sum = 0u64;
        let (w, t) = (warm(2), trials(10));
        let s = measure::<(), _>(w, t, || {
            let mut a = 0u64;
            for j in 0..views {
                let st = (j * 4099) % span;
                let v: &[u8] = &d[st..st + 64];
                a += v[0] as u64;
            }
            sum = std::hint::black_box(a);
            Ok(())
        }).unwrap();
        emit("buffer-slice", views as i64, "&[u8]", &c.ver, w, t, &s, sum == exp,
             &format!("check={sum}; {views} views of 64 bytes{}", sm("1e7")), "views/s");
    }
}

// ------------------------------------------------------------ files
fn file_rows(c: &Ctx) {
    let dev = format!("; device {}", c.dev);
    let mib = seq_bytes() / 1_048_576;
    let seq = c.p(&format!("io-seq-{}.bin", c.scale));
    if c.want("file-read-sequential") {
        report("file-read-sequential", "std::fs", &c.ver, || {
            let mut buf = vec![0u8; 1_048_576];
            let (mut bytes, mut sum) = (0u64, 0u64);
            let (w, t) = (warm(1), trials(5));
            let s = measure(w, t, || -> io::Result<()> {
                let mut f = File::open(&seq)?;
                let (mut pos, mut a) = (0u64, 0u64);
                loop {
                    let n = f.read(&mut buf)?;
                    if n == 0 { break; }
                    let mut i = ((4096 - (pos & 4095)) & 4095) as usize;
                    while i < n { a += buf[i] as u64; i += 4096; }
                    pos += n as u64;
                }
                bytes = pos; sum = a;
                Ok(())
            })?;
            emit("file-read-sequential", mib as i64, "std::fs", &c.ver, w, t, &s, bytes == seq_bytes() && sum == mib * 32640,
                 &format!("check={bytes}/{sum}{dev}; {mib} MiB in 1 MiB reads{}; page cache warm (no drop without root); {NOTE_ERR}", sm("1 GiB")), "MB/s");
            Ok(())
        });
    }
    if c.want("file-write-sequential") {
        report("file-write-sequential", "std::fs", &c.ver, || {
            let chunk: Vec<u8> = (0..1_048_576usize).map(|j| ((((j >> 12) * 31) + j) & 255) as u8).collect();
            let out = c.p("io-write.bin");
            let mut size = 0u64;
            let (w, t) = (warm(1), trials(5));
            let s = measure(w, t, || -> io::Result<()> {
                let mut f = File::create(&out)?;
                for _ in 0..mib { f.write_all(&chunk)?; }
                f.sync_all()?;
                drop(f);
                size = fs::metadata(&out)?.len();
                Ok(())
            })?;
            emit("file-write-sequential", mib as i64, "std::fs", &c.ver, w, t, &s, size == seq_bytes(),
                 &format!("check={size}{dev}; {mib} MiB in 1 MiB writes + fsync{}; {NOTE_ERR}", sm("1 GiB")), "MB/s");
            Ok(())
        });
    }
    if c.want("file-random-read") {
        report("file-random-read", "std::fs", &c.ver, || {
            let reads = pick(100_000, 10_000);
            let blocks = seq_bytes() / 4096;
            let mut x: u64 = 42;
            let mut exp = 0u64;
            for _ in 0..reads {
                x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                let m = (x >> 33) % blocks;
                exp += ((m * 31) & 255) + ((m * 31 + 4095) & 255);
            }
            let mut buf = vec![0u8; 4096];
            let mut sum = 0u64;
            let (w, t) = (warm(2), trials(10));
            let s = measure(w, t, || -> io::Result<()> {
                let f = File::open(&seq)?;
                let mut y: u64 = 42;
                let mut a = 0u64;
                for _ in 0..reads {
                    y = y.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                    let m = (y >> 33) % blocks;
                    f.read_exact_at(&mut buf, m * 4096)?;
                    a += buf[0] as u64 + buf[4095] as u64;
                }
                sum = a;
                Ok(())
            })?;
            emit("file-random-read", reads as i64, "std::fs FileExt::read_exact_at", &c.ver, w, t, &s, sum == exp,
                 &format!("check={sum}{dev}; {reads} reads of 4 KiB over {mib} MiB{}; {NOTE_ERR}", sm("1e5 over 1 GiB")), "reads/s");
            Ok(())
        });
    }
    if c.want("file-stat") {
        report("file-stat", "std::fs::metadata", &c.ver, || {
            let n = pick(1_000_000, 100_000);
            let sp = c.p("io-stat.bin");
            let mut sum = 0u64;
            let (w, t) = (warm(2), trials(10));
            let s = measure(w, t, || -> io::Result<()> {
                let mut a = 0u64;
                for _ in 0..n { a += fs::metadata(&sp)?.len(); }
                sum = a;
                Ok(())
            })?;
            emit("file-stat", n as i64, "std::fs::metadata", &c.ver, w, t, &s, sum == n * 4096,
                 &format!("check={sum}{dev}; {n} size queries{}; {NOTE_ERR}", sm("1e6")), "stats/s");
            Ok(())
        });
    }
    if c.want("filereader-lines") {
        report("filereader-lines", "BufRead::lines", &c.ver, || {
            let lp = c.p(&format!("io-lines-{}.txt", c.scale));
            let mut lines = 0u64;
            let (w, t) = (warm(1), trials(5));
            let s = measure(w, t, || -> io::Result<()> {
                let r = BufReader::new(File::open(&lp)?);
                let mut k = 0u64;
                for l in r.lines() { l?; k += 1; }
                lines = k;
                Ok(())
            })?;
            emit("filereader-lines", line_count() as i64, "BufRead::lines", &c.ver, w, t, &s, lines == line_count(),
                 &format!("check={lines}{dev}; {} lines of 50 bytes{}; {NOTE_ERR}", line_count(), sm("1e7")), "lines/s");
            Ok(())
        });
    }
    if c.want("filewriter-lines") {
        report("filewriter-lines", "BufWriter", &c.ver, || {
            let op = c.p("io-lines-out.txt");
            let n = line_count();
            let mut size = 0u64;
            let (w, t) = (warm(1), trials(5));
            let s = measure(w, t, || -> io::Result<()> {
                let mut out = BufWriter::new(File::create(&op)?);
                let mut ln = [0u8; 50];
                for k in 0..n {
                    let mut v = k;
                    for d in (0..10).rev() { ln[d] = b'0' + (v % 10) as u8; v /= 10; }
                    ln[10..49].fill(b'a' + (k % 26) as u8);
                    ln[49] = b'\n';
                    out.write_all(&ln)?;
                }
                out.flush()?;
                drop(out);
                size = fs::metadata(&op)?.len();
                Ok(())
            })?;
            emit("filewriter-lines", n as i64, "BufWriter", &c.ver, w, t, &s, size == n * 50,
                 &format!("check={size}{dev}; {n} lines{}; {NOTE_ERR}", sm("1e7")), "lines/s");
            Ok(())
        });
    }
    if c.want("directory-walk") {
        report("directory-walk", "walkdir", &lock_version("walkdir"), || {
            let root = c.p(&format!("tree-{}", c.scale));
            let depth = if smoke() { 2 } else { 3 };
            let (mut dirs, mut lvl) = (0u64, 1u64);
            for _ in 0..depth { lvl *= 10; dirs += lvl; }
            let per = dirs + (dirs + 1) * 5;
            let mut entries = 0u64;
            let (w, t) = (warm(2), trials(10));
            let s = measure(w, t, || -> io::Result<()> {
                let mut k = 0u64;
                for _ in 0..10 {
                    for e in walkdir::WalkDir::new(&root).min_depth(1) { e?; k += 1; }
                }
                entries = k;
                Ok(())
            })?;
            emit("directory-walk", (per * 10) as i64, "walkdir", &lock_version("walkdir"), w, t, &s, entries == per * 10,
                 &format!("check={entries}{dev}; {per} entries x 10{}; {NOTE_ERR}",
                          if smoke() { " (smoke tree: depth 2; the page's depth-3 tree is the full run)" } else { "" }), "entries/s");
            Ok(())
        });
    }
}

// ------------------------------------------------------------ path
fn normalize(p: &Path) -> PathBuf {
    // std has no lexical normalize: drop "." and pop on "..", as the others do.
    let mut out = PathBuf::new();
    for comp in p.components() {
        match comp {
            Component::CurDir => {}
            Component::ParentDir => { out.pop(); }
            other => out.push(other.as_os_str()),
        }
    }
    out
}

fn path_rows(c: &Ctx) {
    if !c.want("path-join-normalize") { return; }
    let n = pick(10_000_000, 1_000_000);
    let a: Vec<String> = (0..16).map(|k| format!("d{k}")).collect();
    let b: Vec<String> = (0..8).map(|k| if k % 2 == 0 { format!("../e{k}") } else { format!("f{k}/..") }).collect();
    let cc: Vec<String> = (0..32).map(|k| format!("file{k}.txt")).collect();
    let mut exp = 0u64;
    for i in 0..n {
        let seg = if (i & 7) % 2 == 0 { 2 } else if (i & 15) < 10 { 2 } else { 3 };
        exp += 9 + seg + 1 + if (i & 31) < 10 { 9 } else { 10 };
    }
    let base = PathBuf::from("/srv/app");
    let mut sum = 0u64;
    let (w, t) = (warm(2), trials(10));
    let s = measure::<(), _>(w, t, || {
        let mut acc = 0u64;
        for i in 0..n as usize {
            let p = base.join(&a[i & 15]).join(&b[i & 7]).join(&cc[i & 31]);
            acc += normalize(&p).as_os_str().len() as u64;
        }
        sum = acc;
        Ok(())
    }).unwrap();
    emit("path-join-normalize", n as i64, "PathBuf", &c.ver, w, t, &s, sum == exp,
         &format!("check={sum}; {n} joins{}; PathBuf::join + a components() normalize (std has none)", sm("1e7")), "joins/s");
}

// ------------------------------------------------------------ watcher
fn watcher_rows(c: &Ctx) {
    if !c.want("watcher-event-latency") { return; }
    let ver = lock_version("notify");
    report("watcher-event-latency", "notify", &ver, || {
        use notify::{EventKind, RecursiveMode, Watcher};
        let n = pick(1000, 100) as usize;
        let wd = c.p("watch-rust");
        let _ = fs::remove_dir_all(&wd);
        fs::create_dir_all(&wd)?;
        let (tx, rx) = mpsc::channel();
        let mut watcher = notify::recommended_watcher(tx).map_err(io::Error::other)?;
        watcher.watch(Path::new(&wd), RecursiveMode::NonRecursive).map_err(io::Error::other)?;
        let mut lat: Vec<u128> = Vec::with_capacity(n);
        let mut events = 0usize;
        for k in 0..n {
            let target = PathBuf::from(format!("{wd}/f{k}"));
            let t0 = Instant::now();
            File::create(&target)?;
            let deadline = t0 + Duration::from_secs(2);
            loop {
                let left = deadline.saturating_duration_since(Instant::now());
                match rx.recv_timeout(left) {
                    Ok(Ok(ev)) => {
                        if matches!(ev.kind, EventKind::Create(_)) && ev.paths.iter().any(|p| p == &target) {
                            lat.push(t0.elapsed().as_nanos());
                            events += 1;
                            break;
                        }
                    }
                    Ok(Err(e)) => return Err(io::Error::other(e)),
                    Err(_) => break,
                }
            }
        }
        drop(watcher);
        let _ = fs::remove_dir_all(&wd);
        let s = if lat.is_empty() { Stats { min: 0, median: 0, mean: 0, p95: 0 } } else { stats_of(lat) };
        emit("watcher-event-latency", n as i64, "notify", &ver, 0, events, &s, events == n,
             &format!("check={events}; {n} file creations; median create-to-event latency{}; inotify backend; {NOTE_ERR}", sm("1e3")), "");
        Ok(())
    });
}

// ------------------------------------------------------------ bytes
fn bytes_rows(c: &Ctx) {
    let ver = lock_version("bytes");
    if c.want("bytebuffer-put-get") {
        let pairs = pick(25_000_000, 2_500_000);
        let mut exp: i64 = 0;
        for j in 0..pairs {
            exp = exp.wrapping_add((j.wrapping_mul(2654435761) as u32) as i32 as i64).wrapping_add(j.wrapping_mul(0x9E3779B97F4A7C15) as i64);
        }
        let mut buf = BytesMut::with_capacity(65536);
        let mut sum = 0i64;
        let (w, t) = (warm(2), trials(10));
        let s = measure::<(), _>(w, t, || {
            let mut acc: i64 = 0;
            let mut j = 0u64;
            while j < pairs {
                let end = (j + 5000).min(pairs);
                buf.clear();
                for k in j..end {
                    buf.put_i32(k.wrapping_mul(2654435761) as u32 as i32);
                    buf.put_i64(k.wrapping_mul(0x9E3779B97F4A7C15) as i64);
                }
                let mut r = &buf[..];
                for _ in j..end {
                    acc = acc.wrapping_add(r.get_i32() as i64).wrapping_add(r.get_i64());
                }
                j = end;
            }
            sum = std::hint::black_box(acc);
            Ok(())
        }).unwrap();
        emit("bytebuffer-put-get", (pairs * 4) as i64, "bytes", &ver, w, t, &s, sum == exp,
             &format!("check={sum}; {pairs} int32+int64 pairs put and got (big-endian){}; BufMut::put_i32/put_i64 + Buf::get_*", sm("1e8 ops")), "ops/s");
    }
    if c.want("bufferpool-acquire-release") {
        let n = pick(10_000_000, 1_000_000);
        let mut pool: Vec<BytesMut> = Vec::new();
        let mut allocations = 0u64;
        let mut good = 0u64;
        let (w, t) = (warm(2), trials(10));
        let s = measure::<(), _>(w, t, || {
            let mut g = 0u64;
            for i in 0..n {
                let mut b = match pool.pop() { Some(b) => b, None => { allocations += 1; BytesMut::with_capacity(65536) } };
                b.clear();
                let x = (i & 127) as u8;
                b.put_u8(x);
                if b.capacity() >= 65536 && b[0] == x { g += 1; }
                if pool.len() < 64 { pool.push(b); }
            }
            good = std::hint::black_box(g);
            Ok(())
        }).unwrap();
        emit("bufferpool-acquire-release", n as i64, "bytes::BytesMut reuse", &ver, w, t, &s, good == n && allocations == 1,
             &format!("check={good}; allocations={allocations}; {n} acquire/release pairs{}; a Vec<BytesMut> free list (64 idle max)", sm("1e7")), "acquires/s");
    }
}

// ------------------------------------------------------------ net (std, peer on a thread)
fn lo(port: u16) -> SocketAddr { SocketAddr::from(([127, 0, 0, 1], port)) }

fn net_rows(c: &Ctx) {
    if c.want("tcp-echo-latency") {
        report("tcp-echo-latency", "std::net", &c.ver, || {
            let rounds = pick(100_000, 1000) as usize;
            let port = PORT_BASE;
            let mut rtt = vec![0u128; rounds];
            let mut okc = 0usize;
            let (w, t) = (warm(1), trials(3));
            let s = measure(w, t, || -> io::Result<()> {
                let l = TcpListener::bind(lo(port))?;
                let peer = thread::spawn(move || -> io::Result<()> {
                    let (mut sck, _) = l.accept()?;
                    sck.set_nodelay(true)?;
                    let mut b = vec![0u8; 65536];
                    loop { let n = sck.read(&mut b)?; if n == 0 { break; } sck.write_all(&b[..n])?; }
                    Ok(())
                });
                let mut cl = TcpStream::connect(lo(port))?;
                cl.set_nodelay(true)?;
                let (mut msg, mut back) = ([0u8; 64], [0u8; 64]);
                let mut ok = 0;
                for r in 0..rounds {
                    for i in 0..64 { msg[i] = ((r + i) & 255) as u8; }
                    let t0 = Instant::now();
                    cl.write_all(&msg)?;
                    cl.read_exact(&mut back)?;
                    rtt[r] = t0.elapsed().as_nanos();
                    if back == msg { ok += 1; }
                }
                cl.shutdown(Shutdown::Both)?;
                drop(cl);
                peer.join().map_err(|_| io::Error::other("peer panicked"))??;
                okc = ok;
                Ok(())
            })?;
            let r = stats_of(rtt.clone());
            emit("tcp-echo-latency", rounds as i64, "std::net", &c.ver, w, t, &s, okc == rounds,
                 &format!("check={okc}; rtt_median_ns={}; rtt_p95_ns={}; {rounds} round trips{}{}; peer on a thread; {NOTE_ERR}",
                          r.median, r.p95, sm("1e5"), pid_note(0)), "round trips/s");
            Ok(())
        });
    }
    if c.want("tcp-bulk-throughput") {
        report("tcp-bulk-throughput", "std::net", &c.ver, || {
            let bytes = pick(4_294_967_296, 268_435_456);
            let port = PORT_BASE + 1;
            let chunk: Vec<u8> = (0..65536).map(|i| (i & 255) as u8).collect();
            let mut got = 0u64;
            let (w, t) = (warm(1), trials(5));
            let s = measure(w, t, || -> io::Result<()> {
                let l = TcpListener::bind(lo(port))?;
                let peer = thread::spawn(move || -> io::Result<u64> {
                    let (mut sck, _) = l.accept()?;
                    let mut b = vec![0u8; 65536];
                    let mut tot = 0u64;
                    loop { let n = sck.read(&mut b)?; if n == 0 { break; } tot += n as u64; }
                    Ok(tot)
                });
                let mut cl = TcpStream::connect(lo(port))?;
                let mut sent = 0u64;
                while sent < bytes { cl.write_all(&chunk)?; sent += 65536; }
                drop(cl);
                got = peer.join().map_err(|_| io::Error::other("peer panicked"))??;
                Ok(())
            })?;
            emit("tcp-bulk-throughput", (bytes / 1_048_576) as i64, "std::net", &c.ver, w, t, &s, got == bytes,
                 &format!("check={got}; {} MiB in 64 KiB writes{}{}; peer on a thread; {NOTE_ERR}", bytes / 1_048_576, sm("4 GiB"), pid_note(1)), "MB/s");
            Ok(())
        });
    }
    if c.want("tcp-accept-rate") {
        report("tcp-accept-rate", "std::net", &c.ver, || {
            let n = pick(100_000, 10_000) as usize;
            let port = PORT_BASE + 2;
            let mut accepted = 0usize;
            let (w, t) = (warm(1), trials(5));
            let s = measure(w, t, || -> io::Result<()> {
                let l = listener_backlog(port, 4096)?;
                let peer = thread::spawn(move || -> io::Result<usize> {
                    let mut made = 0;
                    for _ in 0..n {
                        let s = TcpStream::connect(lo(port))?;
                        set_linger0(&s);
                        drop(s);
                        made += 1;
                    }
                    Ok(made)
                });
                let mut a = 0;
                for _ in 0..n { let (s, _) = l.accept()?; drop(s); a += 1; }
                let made = peer.join().map_err(|_| io::Error::other("peer panicked"))??;
                accepted = if made == a { a } else { 0 };
                Ok(())
            })?;
            emit("tcp-accept-rate", n as i64, "std::net", &c.ver, w, t, &s, accepted == n,
                 &format!("check={accepted}; {n} connections{}{}; connector on a thread; client close with SO_LINGER 0; {NOTE_ERR}", sm("1e5"), pid_note(2)), "connections/s");
            Ok(())
        });
    }
    if c.want("udp-send-rate") {
        report("udp-send-rate", "std::net", &c.ver, || {
            let n = pick(1_000_000, 100_000) as usize;
            let port = PORT_BASE + 3;
            let mut got = 0usize;
            let (w, t) = (warm(1), trials(5));
            let s = measure(w, t, || -> io::Result<()> {
                let rx = UdpSocket::bind(lo(port))?;
                let tx = UdpSocket::bind(lo(port + 1))?;
                let peer = thread::spawn(move || -> io::Result<usize> {
                    let mut b = [0u8; 2048];
                    let mut rec = 0;
                    while rec < n {
                        let (k, _) = rx.recv_from(&mut b)?;
                        if k == 512 { rec += 1; if rec % 64 == 0 || rec == n { rx.send_to(&[1], lo(port + 1))?; } }
                    }
                    Ok(rec)
                });
                let mut d = [0u8; 512];
                let mut ack = [0u8; 8];
                let mut i = 0;
                while i < n {
                    d[0] = (i & 255) as u8;
                    tx.send_to(&d, lo(port))?;
                    i += 1;
                    if i % 64 == 0 || i == n { tx.recv_from(&mut ack)?; }
                }
                got = peer.join().map_err(|_| io::Error::other("peer panicked"))??;
                Ok(())
            })?;
            emit("udp-send-rate", n as i64, "std::net", &c.ver, w, t, &s, got == n,
                 &format!("check={got}; {n} datagrams of 512 bytes acked every 64{}; ports {}-{}; pid {}; receiver on a thread; {NOTE_ERR}",
                          sm("1e6"), PORT_BASE + 3, PORT_BASE + 4, std::process::id()), "datagrams/s");
            Ok(())
        });
    }
}

fn listener_backlog(port: u16, backlog: i32) -> io::Result<TcpListener> {
    // std's TcpListener::bind listens with a backlog of 128; the accept-rate
    // row's connector would stall behind it, so set it explicitly (libc).
    use std::os::fd::{FromRawFd, IntoRawFd};
    let l = TcpListener::bind(lo(port))?;
    let fd = l.into_raw_fd();
    unsafe { libc_listen(fd, backlog); }
    Ok(unsafe { TcpListener::from_raw_fd(fd) })
}
extern "C" { #[link_name = "listen"] fn libc_listen(fd: i32, backlog: i32) -> i32; }
extern "C" { fn setsockopt(fd: i32, level: i32, name: i32, val: *const u8, len: u32) -> i32; }
fn set_linger0(s: &TcpStream) {
    use std::os::fd::AsRawFd;
    let lg: [i32; 2] = [1, 0]; // struct linger { l_onoff = 1, l_linger = 0 }
    unsafe { setsockopt(s.as_raw_fd(), 1 /* SOL_SOCKET */, 13 /* SO_LINGER */, lg.as_ptr() as *const u8, 8); }
}

// ------------------------------------------------------------ url
const URIS: [&str; 16] = [
    "https://example.com:443/a/b/c?x=1&y=two#frag",
    "http://api.example.org:8080/v1/users/42?fields=name%2Cemail&limit=10",
    "https://cdn.example.net:443/assets/img%20one.png?w=640&h=480",
    "http://localhost:3000/search?q=caf%C3%A9&page=2#results",
    "https://user@host.example.com:8443/path/to/resource?a=%2F%3F&b=c%26d",
    "ftp://files.example.com:21/pub/file.tar.gz",
    "https://www.example.com:443/?utm_source=news&utm_medium=email&utm_campaign=fall%202026",
    "http://10.0.0.1:8000/metrics?name=cpu%5Ftotal&le=0.5",
    "https://shop.example.com:443/cart/items/9?coupon=SAVE%2010&ref=home#top",
    "http://example.com:80/a%2Fb/c%3Bd?k=v",
    "https://docs.example.com:443/guide/intro.html#section-2",
    "http://svc.internal:9090/rpc?method=get&id=%7B1%7D",
    "https://maps.example.com:443/place?ll=37.77%2C-122.41&z=12",
    "http://example.com:8081/~user/index.html?lang=en-US",
    "https://auth.example.com:443/oauth/authorize?client_id=abc&redirect_uri=https%3A%2F%2Fapp.example.com%2Fcb&scope=read%20write",
    "http://example.com:8082/path;params?x=%41%42%43#f%20g",
];
const SEGS: [&str; 8] = ["report 2026", "café menu", "a<b>c", "plain", "say \"hi\"", "x{y}z", "100% done", "über"];
const VALS: [&str; 8] = ["alpha", "a<b", "50%", "café", "x\"y", "{id}", "beta-2", "naïve"];

fn uri_checksum(u: &url::Url) -> u64 {
    let mut c = u.scheme().len() as u64 + u.host_str().map_or(0, |h| h.len()) as u64
        + u.port_or_known_default().unwrap_or(0) as u64 + u.path().len() as u64
        + u.query().map_or(0, |q| q.len()) as u64 + u.fragment().map_or(0, |f| f.len()) as u64;
    for (_, v) in u.query_pairs() { c += v.len() as u64; }
    c
}

fn uri_rows(c: &Ctx) {
    let ver = lock_version("url");
    if c.want("uri-parse") {
        report("uri-parse", "url", &ver, || {
            let n = pick(1_000_000, 100_000) as usize;
            let mut per = [0u64; 16];
            for k in 0..16 { per[k] = uri_checksum(&url::Url::parse(URIS[k]).map_err(io::Error::other)?); }
            let exp: u64 = (0..n).map(|i| per[i & 15]).sum();
            let mut sum = 0u64;
            let (w, t) = (warm(2), trials(10));
            let s = measure(w, t, || -> io::Result<()> {
                let mut a = 0u64;
                for i in 0..n { a += uri_checksum(&url::Url::parse(URIS[i & 15]).map_err(io::Error::other)?); }
                sum = a;
                Ok(())
            })?;
            emit("uri-parse", n as i64, "url", &ver, w, t, &s, sum == exp,
                 &format!("check={sum}; {n} URIs{}; Url::parse + query_pairs; {NOTE_ERR}", sm("1e6")), "URIs/s");
            Ok(())
        });
    }
    if c.want("uri-build") {
        report("uri-build", "url", &ver, || {
            use percent_encoding::{utf8_percent_encode, AsciiSet, NON_ALPHANUMERIC};
            const ENC: &AsciiSet = &NON_ALPHANUMERIC.remove(b'-').remove(b'.').remove(b'_').remove(b'~');
            let n = pick(1_000_000, 100_000) as usize;
            let hosts: Vec<String> = (0..8).map(|k| format!("api{k}.example.com")).collect();
            let base = url::Url::parse("https://localhost/").map_err(io::Error::other)?;
            let one = |i: usize| -> io::Result<u64> {
                let mut u = base.clone();
                u.set_host(Some(&hosts[i & 7])).map_err(io::Error::other)?;
                u.set_port(Some(8000 + (i & 15) as u16)).map_err(|_| io::Error::other("set_port"))?;
                u.set_path(&format!("/v1/{}", utf8_percent_encode(SEGS[i & 7], ENC)));
                u.set_query(Some(&format!("q={}", utf8_percent_encode(VALS[(i >> 3) & 7], ENC))));
                Ok(u.as_str().len() as u64)
            };
            let mut per = [0u64; 64];
            for k in 0..64 { per[k] = one(k)?; }
            let exp: u64 = (0..n).map(|i| per[i & 63]).sum();
            let mut sum = 0u64;
            let (w, t) = (warm(2), trials(10));
            let s = measure(w, t, || -> io::Result<()> {
                let mut a = 0u64;
                for i in 0..n { a += one(i)?; }
                sum = a;
                Ok(())
            })?;
            emit("uri-build", n as i64, "url", &ver, w, t, &s, sum == exp,
                 &format!("check={sum}; {n} URIs{}; set_host/set_port/set_path/set_query with percent-encoding of the parts; {NOTE_ERR}", sm("1e6")), "URIs/s");
            Ok(())
        });
    }
}

fn main() {
    let c = Ctx {
        only: std::env::args().skip(1).collect(),
        dir: env("CAJETA_BENCH_IO_DIR", "build/io-data"),
        scale: if smoke() { "smoke" } else { "full" },
        dev: env("IO_DEVICE", "unknown"),
        ver: String::new(),
    };
    buffer_rows(&c);
    file_rows(&c);
    path_rows(&c);
    watcher_rows(&c);
    bytes_rows(&c);
    net_rows(&c);
    uri_rows(&c);
}
