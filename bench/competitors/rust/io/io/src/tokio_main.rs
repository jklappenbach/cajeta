// io/ competitor, Rust tokio (bench/io/net/TcpStream.md, TcpListener.md,
// Server.md): the multi-threaded runtime, the peer and the Server rows'
// connections on tasks. Sizes, ports (61400+) and checks are those of
// competitors/drivers/io/io.sh. Errors propagate as Result (an invalid row
// with the error), never a panic. Usage: io-tokio [id ...].
mod common;
use common::*;
use std::io;
use std::net::SocketAddr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::Arc;
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{TcpListener, TcpSocket, TcpStream};

const PORT_BASE: u16 = 61400;
const NOTE_ERR: &str = "errors propagate as Result";

fn lo(port: u16) -> SocketAddr { SocketAddr::from(([127, 0, 0, 1], port)) }
fn pid_note(slot: u16) -> String { format!("; port {}; pid {}", PORT_BASE + slot, std::process::id()) }

fn listen(port: u16, backlog: u32) -> io::Result<TcpListener> {
    let s = TcpSocket::new_v4()?;
    s.set_reuseaddr(true)?;
    s.bind(lo(port))?;
    s.listen(backlog)
}

// `w` untimed then `t` timed runs of one trial; returns the stats and the
// last trial's result.
macro_rules! timed {
    ($w:expr, $t:expr, $body:expr) => {{
        let mut v = Vec::new();
        let mut last = None;
        for i in 0..($w + $t.max(1)) {
            let t0 = Instant::now();
            let r = $body.await?;
            if i >= $w { v.push(t0.elapsed().as_nanos()); }
            last = Some(r);
        }
        (stats_of(v), last.unwrap())
    }};
}

async fn echo_once(port: u16, rounds: usize, rtt: &mut [u128]) -> io::Result<usize> {
    let l = listen(port, 128)?;
    let peer = tokio::spawn(async move {
        let (mut s, _) = l.accept().await?;
        s.set_nodelay(true)?;
        let mut b = vec![0u8; 65536];
        loop { let n = s.read(&mut b).await?; if n == 0 { break; } s.write_all(&b[..n]).await?; }
        Ok::<(), io::Error>(())
    });
    let mut c = TcpStream::connect(lo(port)).await?;
    c.set_nodelay(true)?;
    let (mut msg, mut back) = ([0u8; 64], [0u8; 64]);
    let mut ok = 0;
    for r in 0..rounds {
        for i in 0..64 { msg[i] = ((r + i) & 255) as u8; }
        let t0 = Instant::now();
        c.write_all(&msg).await?;
        c.read_exact(&mut back).await?;
        rtt[r] = t0.elapsed().as_nanos();
        if back == msg { ok += 1; }
    }
    drop(c);
    peer.await.map_err(io::Error::other)??;
    Ok(ok)
}

async fn echo_latency(ver: &str) -> io::Result<()> {
    let rounds = pick(100_000, 1000) as usize;
    let mut rtt = vec![0u128; rounds];
    let (w, t) = (warm(1), trials(3));
    let (s, okc) = timed!(w, t, echo_once(PORT_BASE, rounds, &mut rtt));
    let r = stats_of(rtt.clone());
    emit("tcp-echo-latency", rounds as i64, "tokio", ver, w, t, &s, okc == rounds,
         &format!("check={okc}; rtt_median_ns={}; rtt_p95_ns={}; {rounds} round trips{}{}; peer on a task; {NOTE_ERR}",
                  r.median, r.p95, sm("1e5"), pid_note(0)), "round trips/s");
    Ok(())
}

async fn bulk_once(port: u16, bytes: u64, chunk: &[u8]) -> io::Result<u64> {
    let l = listen(port, 128)?;
    let peer = tokio::spawn(async move {
        let (mut s, _) = l.accept().await?;
        let mut b = vec![0u8; 65536];
        let mut tot = 0u64;
        loop { let n = s.read(&mut b).await?; if n == 0 { break; } tot += n as u64; }
        Ok::<u64, io::Error>(tot)
    });
    let mut c = TcpStream::connect(lo(port)).await?;
    let mut sent = 0u64;
    while sent < bytes { c.write_all(chunk).await?; sent += 65536; }
    drop(c);
    peer.await.map_err(io::Error::other)?
}

async fn bulk(ver: &str) -> io::Result<()> {
    let bytes = pick(4_294_967_296, 268_435_456);
    let chunk: Vec<u8> = (0..65536).map(|i| (i & 255) as u8).collect();
    let (w, t) = (warm(1), trials(5));
    let (s, got) = timed!(w, t, bulk_once(PORT_BASE + 1, bytes, &chunk));
    emit("tcp-bulk-throughput", (bytes / 1_048_576) as i64, "tokio", ver, w, t, &s, got == bytes,
         &format!("check={got}; {} MiB in 64 KiB writes{}{}; peer on a task; {NOTE_ERR}", bytes / 1_048_576, sm("4 GiB"), pid_note(1)), "MB/s");
    Ok(())
}

async fn accept_once(port: u16, n: usize) -> io::Result<usize> {
    let l = listen(port, 4096)?;
    let peer = tokio::spawn(async move {
        let mut made = 0usize;
        for _ in 0..n {
            let s = TcpStream::connect(lo(port)).await?;
            #[allow(deprecated)] // linger 0 resets at once; it never blocks the thread
            s.set_linger(Some(Duration::ZERO))?;
            drop(s);
            made += 1;
        }
        Ok::<usize, io::Error>(made)
    });
    let mut a = 0usize;
    for _ in 0..n { let (s, _) = l.accept().await?; drop(s); a += 1; }
    let made = peer.await.map_err(io::Error::other)??;
    Ok(if made == a { a } else { 0 })
}

async fn accept_rate(ver: &str) -> io::Result<()> {
    let n = pick(100_000, 10_000) as usize;
    let (w, t) = (warm(1), trials(5));
    let (s, accepted) = timed!(w, t, accept_once(PORT_BASE + 2, n));
    emit("tcp-accept-rate", n as i64, "tokio", ver, w, t, &s, accepted == n,
         &format!("check={accepted}; {n} connections{}{}; connector on a task; client close with SO_LINGER 0; {NOTE_ERR}", sm("1e5"), pid_note(2)), "connections/s");
    Ok(())
}

async fn echo_server_once(port: u16, clients: u64, dur: Duration) -> io::Result<(u64, u64, u64)> {
    let l = listen(port, 4096)?;
    let served = Arc::new(AtomicU64::new(0));
    let sv = served.clone();
    let server = tokio::spawn(async move {
        let mut handlers = Vec::new();
        for _ in 0..clients {
            let (mut s, _) = l.accept().await?;
            sv.fetch_add(1, Ordering::Relaxed);
            handlers.push(tokio::spawn(async move {
                s.set_nodelay(true)?;
                let mut b = vec![0u8; 16384];
                loop { let n = s.read(&mut b).await?; if n == 0 { break; } s.write_all(&b[..n]).await?; }
                Ok::<(), io::Error>(())
            }));
        }
        for h in handlers { h.await.map_err(io::Error::other)??; }
        Ok::<(), io::Error>(())
    });
    let deadline = Instant::now() + dur;
    let mut cs = Vec::new();
    for id in 0..clients {
        cs.push(tokio::spawn(async move {
            let mut c = TcpStream::connect(lo(port)).await?;
            c.set_nodelay(true)?;
            let (mut msg, mut back) = (vec![0u8; 1024], vec![0u8; 1024]);
            let (mut ok, mut wrong, mut m) = (0u64, 0u64, 0u64);
            while Instant::now() < deadline {
                for i in 0..1024 { msg[i] = ((i as u64 + id + m) & 255) as u8; }
                c.write_all(&msg).await?;
                c.read_exact(&mut back).await?;
                if back == msg { ok += 1 } else { wrong += 1 }
                m += 1;
            }
            Ok::<(u64, u64), io::Error>((ok, wrong))
        }));
    }
    let (mut a, mut b) = (0u64, 0u64);
    for c in cs { let (x, y) = c.await.map_err(io::Error::other)??; a += x; b += y; }
    server.await.map_err(io::Error::other)??;
    Ok((a, b, served.load(Ordering::Relaxed)))
}

async fn echo_server(ver: &str) -> io::Result<()> {
    let clients = 64u64;
    let dur = Duration::from_nanos(pick(10_000_000_000, 2_000_000_000));
    let (w, t) = (warm(1), trials(3));
    let (s, (msgs, bad, conns)) = timed!(w, t, echo_server_once(PORT_BASE + 6, clients, dur));
    emit("server-echo-throughput", msgs as i64, "tokio", ver, w, t, &s, msgs > 0 && bad == 0 && conns == clients,
         &format!("messages={msgs}; mismatches={bad}; connections={conns}; {clients} clients x {} ms{}{}; clients on tasks in-process; {NOTE_ERR}",
                  dur.as_millis(), sm("10 s"), pid_note(6)), "messages/s");
    Ok(())
}

async fn idle_once(port: u16, n: u64, hold: Duration) -> io::Result<(u64, i64, i64)> {
    let l = listen(port, 4096)?;
    let conns = Arc::new(AtomicU64::new(0));
    let cn = conns.clone();
    let server = tokio::spawn(async move {
        let mut hs = Vec::new();
        for _ in 0..n {
            let (mut s, _) = l.accept().await?;
            cn.fetch_add(1, Ordering::Relaxed);
            hs.push(tokio::spawn(async move {
                let mut b = [0u8; 64];
                while s.read(&mut b).await? > 0 {}
                Ok::<(), io::Error>(())
            }));
        }
        for h in hs { h.await.map_err(io::Error::other)??; }
        Ok::<(), io::Error>(())
    });
    let mut cs = Vec::new();
    for _ in 0..n { cs.push(TcpStream::connect(lo(port)).await?); }
    let until = Instant::now() + Duration::from_secs(10);
    while conns.load(Ordering::Relaxed) < n && Instant::now() < until { tokio::time::sleep(Duration::from_millis(1)).await; }
    let cpu0 = cpu_ms();
    tokio::time::sleep(hold / 2).await;
    let rss1 = rss_kb();
    tokio::time::sleep(hold - hold / 2).await;
    let cpu = cpu_ms() - cpu0;
    let held = conns.load(Ordering::Relaxed);
    drop(cs);
    server.await.map_err(io::Error::other)??;
    Ok((held, rss1, cpu))
}

async fn idle(ver: &str) -> io::Result<()> {
    let n = 1000u64;
    let hold = Duration::from_nanos(pick(10_000_000_000, 2_000_000_000));
    let (w, t) = (warm(1), trials(3));
    let (s, (held, rss, cpu)) = timed!(w, t, idle_once(PORT_BASE + 5, n, hold));
    emit("server-1k-idle", n as i64, "tokio", ver, w, t, &s, held == n,
         &format!("check={held}; rss_hold_kb={rss}; cpu_ms={cpu}; {n} connections held {} ms{}{}; a task per connection; clients in-process; {NOTE_ERR}",
                  hold.as_millis(), sm("10 s"), pid_note(5)), "");
    Ok(())
}

#[tokio::main]
async fn main() {
    let only: Vec<String> = std::env::args().skip(1).collect();
    let ver = lock_version("tokio");
    macro_rules! row { ($id:expr, $f:expr) => {
        if want(&only, $id) { if let Err(e) = $f.await { emit_err($id, "tokio", &ver, &e); } }
    } }
    row!("tcp-echo-latency", echo_latency(&ver));
    row!("tcp-bulk-throughput", bulk(&ver));
    row!("tcp-accept-rate", accept_rate(&ver));
    row!("server-echo-throughput", echo_server(&ver));
    row!("server-1k-idle", idle(&ver));
}
