"""io/ competitor, Python (bench/io/*.md): bytearray and memoryview, open,
os.stat, line iteration, pathlib, os.scandir, watchdog, asyncio (the
TcpStream, TcpListener and Server rows: peer and clients on the same event
loop), socket with a receiver thread (UDP) and urllib.parse. Sizes, inputs,
ports (61700+) and checks are those of competitors/drivers/io/io.sh; every
row's notes carry check=<value>. Usage: io_bench.py [id ...]."""
import asyncio
import os
import pathlib
import queue
import socket
import struct
import sys
import threading
import time
import urllib.parse

import _rows as R

PORT_BASE = 61700
ONLY = set(sys.argv[1:])
DIR = R.env("CAJETA_BENCH_IO_DIR", "build/io-data")
SCALE = "smoke" if R.smoke() else "full"
DEV = R.env("IO_DEVICE", "unknown")


def want(i):
    return not ONLY or i in ONLY


def P(n):
    return DIR + "/" + n


def pid_note(slot):
    return f"; port {PORT_BASE + slot}; pid {os.getpid()}"


def seq_bytes():
    return R.pick(1073741824, 67108864)


def line_count():
    return R.pick(10_000_000, 1_000_000)


def guard(slug, lib):
    def deco(fn):
        if want(slug):
            try:
                fn()
            except Exception as e:  # noqa: BLE001 - recorded as an invalid row
                R.out(R.err(slug, lib, e))
        return fn
    return deco


N16 = 16777216


# ------------------------------------------------------------- buffer
@guard("buffer-fill", "bytearray")
def buffer_fill():
    passes = R.pick(100, 4)
    exp = sum(((q * 7919) % N16 + q) & 255 for q in range(passes)) + (N16 // 256) * 32640
    box = [0]

    def run():
        d = bytearray(N16)
        a = 0
        for q in range(passes):
            for i in range(N16):
                d[i] = (i + q) & 255
            a += d[(q * 7919) % N16]
        a += sum(d)
        box[0] = a
    w, t = R.warm(1), R.trials(3)
    st = R.measure(run, w, t)
    R.out(R.row("buffer-fill", N16 * passes // 1048576, "bytearray", "", w, t, st, box[0] == exp,
                f"check={box[0]}; 16 MiB x {passes} passes{R.sm('100 passes')}", "MB/s"))


@guard("buffer-read-primitives", "struct.unpack_from")
def buffer_read_primitives():
    reads = R.pick(50_000_000, 5_000_000)
    d = bytearray(N16 + 8)
    for k in range(2097152):
        struct.pack_into("<I", d, k * 4, (k * 2654435761) & 0xFFFFFFFF)
    for k in range(1048576):
        struct.pack_into("<d", d, 8388608 + k * 8, (k & 1023) * 0.5)
    ei = 0
    ef = 0.0
    for j in range(reads):
        v = ((j & 2097151) * 2654435761) & 0xFFFFFFFF
        ei += v - (1 << 32) if v >= 1 << 31 else v
        ef += ((j & 1048575) & 1023) * 0.5
    box = [0, 0.0]
    i32 = struct.Struct("<i").unpack_from
    f64 = struct.Struct("<d").unpack_from

    def run():
        a = 0
        b = 0.0
        for j in range(reads):
            a += i32(d, (j & 2097151) * 4)[0]
            b += f64(d, 8388608 + (j & 1048575) * 8)[0]
        box[0], box[1] = a, b
    w, t = R.warm(1), R.trials(3)
    st = R.measure(run, w, t)
    R.out(R.row("buffer-read-primitives", reads * 2, "struct.unpack_from", "", w, t, st, box[0] == ei and box[1] == ef,
                f"check={box[0]}/{int(box[1] * 2)}; {reads * 2} values{R.sm('1e8')}", "values/s"))


@guard("buffer-slice", "memoryview")
def buffer_slice():
    views = R.pick(10_000_000, 1_000_000)
    span = N16 - 64
    d = bytearray((i * 31 + 7) & 255 for i in range(256)) * (N16 // 256)
    exp = sum((((j * 4099) % span) * 31 + 7) & 255 for j in range(views))
    box = [0]
    mv = memoryview(d)

    def run():
        a = 0
        for j in range(views):
            s = (j * 4099) % span
            a += mv[s:s + 64][0]
        box[0] = a
    w, t = R.warm(1), R.trials(3)
    st = R.measure(run, w, t)
    R.out(R.row("buffer-slice", views, "memoryview", "", w, t, st, box[0] == exp,
                f"check={box[0]}; {views} views of 64 bytes{R.sm('1e7')}", "views/s"))


# ------------------------------------------------------------- files
DV = "; device " + DEV
MIB = seq_bytes() // 1048576
SEQ = P(f"io-seq-{SCALE}.bin")


@guard("file-read-sequential", "open")
def file_read_sequential():
    buf = bytearray(1048576)
    mv = memoryview(buf)
    box = [0, 0]

    def run():
        pos = 0
        a = 0
        with open(SEQ, "rb", buffering=0) as f:
            while True:
                n = f.readinto(buf)
                if not n:
                    break
                a += sum(mv[(4096 - (pos & 4095)) & 4095:n:4096])
                pos += n
        box[0], box[1] = pos, a
    w, t = R.warm(1), R.trials(5)
    st = R.measure(run, w, t)
    R.out(R.row("file-read-sequential", MIB, "open", "", w, t, st, box[0] == seq_bytes() and box[1] == MIB * 32640,
                f"check={box[0]}/{box[1]}{DV}; {MIB} MiB in 1 MiB reads{R.sm('1 GiB')}; page cache warm (no drop without root); readinto", "MB/s"))


@guard("file-write-sequential", "open")
def file_write_sequential():
    chunk = bytes((((j >> 12) * 31) + j) & 255 for j in range(1048576))
    out = P("io-write.bin")
    box = [0]

    def run():
        with open(out, "wb", buffering=0) as f:
            for _ in range(MIB):
                f.write(chunk)
            os.fsync(f.fileno())
        box[0] = os.stat(out).st_size
    w, t = R.warm(1), R.trials(5)
    st = R.measure(run, w, t)
    R.out(R.row("file-write-sequential", MIB, "open", "", w, t, st, box[0] == seq_bytes(),
                f"check={box[0]}{DV}; {MIB} MiB in 1 MiB writes + fsync{R.sm('1 GiB')}", "MB/s"))


@guard("file-random-read", "os.pread")
def file_random_read():
    reads = R.pick(100_000, 10_000)
    blocks = seq_bytes() // 4096
    M = (1 << 64) - 1
    x = 42
    exp = 0
    offs = []
    for _ in range(reads):
        x = (x * 6364136223846793005 + 1442695040888963407) & M
        m = (x >> 33) % blocks
        exp += ((m * 31) & 255) + ((m * 31 + 4095) & 255)
    box = [0]

    def run():
        fd = os.open(SEQ, os.O_RDONLY)
        try:
            y = 42
            a = 0
            for _ in range(reads):
                y = (y * 6364136223846793005 + 1442695040888963407) & M
                b = os.pread(fd, 4096, ((y >> 33) % blocks) * 4096)
                a += b[0] + b[4095]
            box[0] = a
        finally:
            os.close(fd)
    del offs
    w, t = R.warm(1), R.trials(5)
    st = R.measure(run, w, t)
    R.out(R.row("file-random-read", reads, "os.pread", "", w, t, st, box[0] == exp,
                f"check={box[0]}{DV}; {reads} reads of 4 KiB over {MIB} MiB{R.sm('1e5 over 1 GiB')}", "reads/s"))


@guard("file-stat", "os.stat")
def file_stat():
    n = R.pick(1_000_000, 100_000)
    sp = P("io-stat.bin")
    box = [0]

    def run():
        a = 0
        st_ = os.stat
        for _ in range(n):
            a += st_(sp).st_size
        box[0] = a
    w, t = R.warm(1), R.trials(5)
    st = R.measure(run, w, t)
    R.out(R.row("file-stat", n, "os.stat", "", w, t, st, box[0] == n * 4096,
                f"check={box[0]}{DV}; {n} size queries{R.sm('1e6')}", "stats/s"))


@guard("filereader-lines", "file iteration")
def filereader_lines():
    lp = P(f"io-lines-{SCALE}.txt")
    box = [0]

    def run():
        c = 0
        with open(lp, encoding="utf-8") as f:
            for _ in f:
                c += 1
        box[0] = c
    w, t = R.warm(1), R.trials(5)
    st = R.measure(run, w, t)
    R.out(R.row("filereader-lines", line_count(), "file iteration", "", w, t, st, box[0] == line_count(),
                f"check={box[0]}{DV}; {line_count()} lines of 50 bytes{R.sm('1e7')}; text mode (UTF-8 decode)", "lines/s"))


@guard("filewriter-lines", "file.write")
def filewriter_lines():
    op = P("io-lines-out.txt")
    n = line_count()
    letters = [bytes([97 + k]) * 39 + b"\n" for k in range(26)]
    box = [0]

    def run():
        with open(op, "wb") as f:
            wr = f.write
            for k in range(n):
                wr(b"%010d" % k + letters[k % 26])
        box[0] = os.stat(op).st_size
    w, t = R.warm(1), R.trials(5)
    st = R.measure(run, w, t)
    R.out(R.row("filewriter-lines", n, "file.write", "", w, t, st, box[0] == n * 50,
                f"check={box[0]}{DV}; {n} lines{R.sm('1e7')}; buffered binary file; line = %-format + suffix", "lines/s"))


@guard("directory-walk", "os.scandir")
def directory_walk():
    root = P(f"tree-{SCALE}")
    depth = 2 if R.smoke() else 3
    dirs = sum(10 ** l for l in range(1, depth + 1))
    per = dirs + (dirs + 1) * 5
    box = [0]

    def walk(p):
        c = 0
        with os.scandir(p) as it:
            for e in it:
                c += 1
                if e.is_dir(follow_symlinks=False):
                    c += walk(e.path)
        return c

    def run():
        box[0] = sum(walk(root) for _ in range(10))
    w, t = R.warm(1), R.trials(5)
    st = R.measure(run, w, t)
    tree = " (smoke tree: depth 2; the page's depth-3 tree is the full run)" if R.smoke() else ""
    R.out(R.row("directory-walk", per * 10, "os.scandir", "", w, t, st, box[0] == per * 10,
                f"check={box[0]}{DV}; {per} entries x 10{tree}; recursive scandir (d_type)", "entries/s"))


# ------------------------------------------------------------- path
@guard("path-join-normalize", "pathlib")
def path_join_normalize():
    n = R.pick(10_000_000, 1_000_000)
    A = [f"d{k}" for k in range(16)]
    B = [f"../e{k}" if k % 2 == 0 else f"f{k}/.." for k in range(8)]
    C = [f"file{k}.txt" for k in range(32)]
    exp = 0
    for i in range(n):
        seg = 2 if (i & 7) % 2 == 0 else (2 if (i & 15) < 10 else 3)
        exp += 9 + seg + 1 + (9 if (i & 31) < 10 else 10)
    base = pathlib.PurePosixPath("/srv/app")
    norm = os.path.normpath
    box = [0]

    def run():
        a = 0
        for i in range(n):
            a += len(norm(str(base / A[i & 15] / B[i & 7] / C[i & 31])))
        box[0] = a
    w, t = R.warm(1), R.trials(3)
    st = R.measure(run, w, t)
    R.out(R.row("path-join-normalize", n, "pathlib", "", w, t, st, box[0] == exp,
                f"check={box[0]}; {n} joins{R.sm('1e7')}; PurePosixPath / + os.path.normpath (pathlib does not collapse ..)", "joins/s"))


# ------------------------------------------------------------- watcher
@guard("watcher-event-latency", "watchdog")
def watcher_event_latency():
    from watchdog.events import FileSystemEventHandler
    from watchdog.observers import Observer
    import importlib.metadata as md
    n = R.pick(1000, 100)
    wd = P("watch-python")
    import shutil
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    q = queue.Queue()

    class H(FileSystemEventHandler):
        def on_created(self, ev):
            q.put((ev.src_path, time.perf_counter_ns()))
    obs = Observer()
    obs.schedule(H(), wd, recursive=False)
    obs.start()
    lat = []
    try:
        for k in range(n):
            target = f"{wd}/f{k}"
            t0 = time.perf_counter_ns()
            open(target, "wb").close()
            deadline = time.monotonic() + 2.0
            while True:
                left = deadline - time.monotonic()
                if left <= 0:
                    break
                try:
                    path, ts = q.get(timeout=left)
                except queue.Empty:
                    break
                if path == target:
                    lat.append(ts - t0)
                    break
    finally:
        obs.stop()
        obs.join()
        shutil.rmtree(wd, ignore_errors=True)
    s = sorted(lat) or [0]
    st = (s[0], s[len(s) // 2], sum(s) // len(s), s[min(len(s) - 1, len(s) * 95 // 100)])
    R.out(R.row("watcher-event-latency", n, "watchdog", md.version("watchdog"), 0, len(lat), st, len(lat) == n,
                f"check={len(lat)}; {n} file creations; median create-to-event latency{R.sm('1e3')}; inotify observer thread", ""))


# ------------------------------------------------------------- net
def lo(port):
    return ("127.0.0.1", port)


def run_async(coro):
    return asyncio.run(coro)


@guard("tcp-echo-latency", "asyncio")
def tcp_echo_latency():
    rounds = R.pick(100_000, 1000)
    port = PORT_BASE
    rtt = [0] * rounds
    box = [0]

    async def trial():
        async def handle(r, w):
            w.get_extra_info("socket").setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            while True:
                b = await r.read(65536)
                if not b:
                    break
                w.write(b)
                await w.drain()
            w.close()
            await w.wait_closed()
        srv = await asyncio.start_server(handle, *lo(port), reuse_address=True)
        r, w = await asyncio.open_connection(*lo(port))
        w.get_extra_info("socket").setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        ok = 0
        pc = time.perf_counter_ns
        for k in range(rounds):
            msg = bytes(((k + i) & 255) for i in range(64))
            t0 = pc()
            w.write(msg)
            back = await r.readexactly(64)
            rtt[k] = pc() - t0
            ok += back == msg
        w.close()
        await w.wait_closed()
        srv.close()
        await srv.wait_closed()
        box[0] = ok
    w_, t_ = R.warm(1), R.trials(3)
    st = R.measure(lambda: run_async(trial()), w_, t_)
    s = sorted(rtt)
    R.out(R.row("tcp-echo-latency", rounds, "asyncio", "", w_, t_, st, box[0] == rounds,
                f"check={box[0]}; rtt_median_ns={s[len(s) // 2]}; rtt_p95_ns={s[min(len(s) - 1, len(s) * 95 // 100)]}; {rounds} round trips{R.sm('1e5')}{pid_note(0)}; peer on the same event loop (streams)", "round trips/s"))


@guard("tcp-bulk-throughput", "asyncio")
def tcp_bulk_throughput():
    nbytes = R.pick(4294967296, 268435456)
    port = PORT_BASE + 1
    chunk = bytes(i & 255 for i in range(256)) * 256
    box = [0]

    async def trial():
        got = [0]
        done = asyncio.Event()

        async def handle(r, w):
            tot = 0
            while True:
                b = await r.read(65536)
                if not b:
                    break
                tot += len(b)
            got[0] = tot
            w.close()
            done.set()
        srv = await asyncio.start_server(handle, *lo(port), reuse_address=True)
        r, w = await asyncio.open_connection(*lo(port))
        sent = 0
        while sent < nbytes:
            w.write(chunk)
            await w.drain()
            sent += 65536
        w.close()
        await w.wait_closed()
        await done.wait()
        srv.close()
        await srv.wait_closed()
        box[0] = got[0]
    w_, t_ = R.warm(1), R.trials(5)
    st = R.measure(lambda: run_async(trial()), w_, t_)
    R.out(R.row("tcp-bulk-throughput", nbytes // 1048576, "asyncio", "", w_, t_, st, box[0] == nbytes,
                f"check={box[0]}; {nbytes // 1048576} MiB in 64 KiB writes{R.sm('4 GiB')}{pid_note(1)}; peer on the same event loop", "MB/s"))


@guard("tcp-accept-rate", "asyncio")
def tcp_accept_rate():
    n = R.pick(100_000, 10_000)
    port = PORT_BASE + 2
    box = [0]
    linger0 = struct.pack("ii", 1, 0)

    async def trial():
        acc = [0]
        done = asyncio.Event()

        async def handle(r, w):
            acc[0] += 1
            w.close()
            if acc[0] == n:
                done.set()
        srv = await asyncio.start_server(handle, *lo(port), reuse_address=True, backlog=4096)
        made = 0
        for _ in range(n):
            r, w = await asyncio.open_connection(*lo(port))
            w.get_extra_info("socket").setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, linger0)
            w.close()
            made += 1
        await asyncio.wait_for(done.wait(), 30)
        srv.close()
        await srv.wait_closed()
        box[0] = acc[0] if made == acc[0] else 0
    w_, t_ = R.warm(1), R.trials(5)
    st = R.measure(lambda: run_async(trial()), w_, t_)
    R.out(R.row("tcp-accept-rate", n, "asyncio", "", w_, t_, st, box[0] == n,
                f"check={box[0]}; {n} connections{R.sm('1e5')}{pid_note(2)}; connector on the same event loop; client close with SO_LINGER 0", "connections/s"))


@guard("udp-send-rate", "socket")
def udp_send_rate():
    n = R.pick(1_000_000, 100_000)
    port = PORT_BASE + 3
    box = [0]

    def trial():
        rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        rx.bind(lo(port))
        tx.bind(lo(port + 1))
        rec = [0]

        def recv():
            back = lo(port + 1)
            c = 0
            rcv = rx.recv
            while c < n:
                if len(rcv(2048)) == 512:
                    c += 1
                    if c % 64 == 0 or c == n:
                        rx.sendto(b"\x01", back)
            rec[0] = c
        th = threading.Thread(target=recv)
        th.start()
        d = bytearray(512)
        dest = lo(port)
        snd = tx.sendto
        for i in range(1, n + 1):
            d[0] = (i - 1) & 255
            snd(d, dest)
            if i % 64 == 0 or i == n:
                tx.recv(8)
        th.join()
        rx.close()
        tx.close()
        box[0] = rec[0]
    w_, t_ = R.warm(1), R.trials(5)
    st = R.measure(trial, w_, t_)
    R.out(R.row("udp-send-rate", n, "socket", "", w_, t_, st, box[0] == n,
                f"check={box[0]}; {n} datagrams of 512 bytes acked every 64{R.sm('1e6')}; ports {PORT_BASE + 3}-{PORT_BASE + 4}; pid {os.getpid()}; receiver on a thread", "datagrams/s"))


@guard("server-echo-throughput", "asyncio")
def server_echo_throughput():
    clients = 64
    dur = R.pick(10.0, 2.0)
    port = PORT_BASE + 6
    box = [0, 0, 0]

    async def trial():
        served = [0]

        async def handle(r, w):
            served[0] += 1
            w.get_extra_info("socket").setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            while True:
                b = await r.read(16384)
                if not b:
                    break
                w.write(b)
                await w.drain()
            w.close()
        srv = await asyncio.start_server(handle, *lo(port), reuse_address=True, backlog=4096)
        deadline = time.monotonic() + dur

        async def client(cid):
            r, w = await asyncio.open_connection(*lo(port))
            w.get_extra_info("socket").setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            ok = bad = m = 0
            base = bytes(range(256)) * 8
            while time.monotonic() < deadline:
                o = (cid + m) & 255
                msg = (base[o:] + base[:o])[:1024]
                w.write(msg)
                back = await r.readexactly(1024)
                if back == msg:
                    ok += 1
                else:
                    bad += 1
                m += 1
            w.close()
            await w.wait_closed()
            return ok, bad
        res = await asyncio.gather(*(client(i) for i in range(clients)))
        srv.close()
        await srv.wait_closed()
        box[0] = sum(a for a, _ in res)
        box[1] = sum(b for _, b in res)
        box[2] = served[0]
    w_, t_ = R.warm(1), R.trials(3)
    st = R.measure(lambda: run_async(trial()), w_, t_)
    R.out(R.row("server-echo-throughput", box[0], "asyncio", "", w_, t_, st, box[0] > 0 and box[1] == 0 and box[2] == clients,
                f"messages={box[0]}; mismatches={box[1]}; connections={box[2]}; {clients} clients x {int(dur * 1000)} ms{R.sm('10 s')}{pid_note(6)}; start_server streams; clients on the same event loop", "messages/s"))


@guard("server-1k-idle", "asyncio")
def server_1k_idle():
    n = 1000
    hold = R.pick(10.0, 2.0)
    port = PORT_BASE + 5
    box = [0, 0, 0]

    async def trial():
        conns = [0]

        async def handle(r, w):
            conns[0] += 1
            while await r.read(64):
                pass
            w.close()
        srv = await asyncio.start_server(handle, *lo(port), reuse_address=True, backlog=4096)
        cs = [await asyncio.open_connection(*lo(port)) for _ in range(n)]
        until = time.monotonic() + 10
        while conns[0] < n and time.monotonic() < until:
            await asyncio.sleep(0.001)
        c0 = R.cpu_ms()
        await asyncio.sleep(hold / 2)
        r1 = R.rss_kb()
        await asyncio.sleep(hold - hold / 2)
        box[0], box[1], box[2] = conns[0], r1, R.cpu_ms() - c0
        for _, w in cs:
            w.close()
        srv.close()
        await srv.wait_closed()
    w_, t_ = R.warm(1), R.trials(3)
    st = R.measure(lambda: run_async(trial()), w_, t_)
    R.out(R.row("server-1k-idle", n, "asyncio", "", w_, t_, st, box[0] == n,
                f"check={box[0]}; rss_hold_kb={box[1]}; cpu_ms={box[2]}; {n} connections held {int(hold * 1000)} ms{R.sm('10 s')}{pid_note(5)}; start_server streams; clients in-process", ""))


# ------------------------------------------------------------- uri
URIS = [
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
]
SEGS = ["report 2026", "café menu", "a<b>c", "plain", 'say "hi"', "x{y}z", "100% done", "über"]
VALS = ["alpha", "a<b", "50%", "café", 'x"y', "{id}", "beta-2", "naïve"]


def uri_checksum(s):
    u = urllib.parse.urlsplit(s)
    c = len(u.scheme) + len(u.hostname or "") + (u.port or 0) + len(u.path) + len(u.query) + len(u.fragment)
    for _, v in urllib.parse.parse_qsl(u.query, keep_blank_values=True):
        c += len(v.encode("utf-8"))
    return c


@guard("uri-parse", "urllib.parse")
def uri_parse():
    n = R.pick(1_000_000, 100_000)
    per = [uri_checksum(s) for s in URIS]
    exp = sum(per[i & 15] for i in range(n))
    box = [0]

    def run():
        box[0] = sum(uri_checksum(URIS[i & 15]) for i in range(n))
    w, t = R.warm(1), R.trials(3)
    st = R.measure(run, w, t)
    R.out(R.row("uri-parse", n, "urllib.parse", "", w, t, st, box[0] == exp,
                f"check={box[0]}; {n} URIs{R.sm('1e6')}; urlsplit + parse_qsl", "URIs/s"))


@guard("uri-build", "urllib.parse")
def uri_build():
    n = R.pick(1_000_000, 100_000)
    hosts = [f"api{k}.example.com" for k in range(8)]
    q = urllib.parse.quote
    unsplit = urllib.parse.urlunsplit

    def one(i):
        return len(unsplit(("https", f"{hosts[i & 7]}:{8000 + (i & 15)}", "/v1/" + q(SEGS[i & 7], safe=""),
                            "q=" + q(VALS[(i >> 3) & 7], safe=""), "")))
    per = [one(k) for k in range(64)]
    exp = sum(per[i & 63] for i in range(n))
    box = [0]

    def run():
        box[0] = sum(one(i) for i in range(n))
    w, t = R.warm(1), R.trials(3)
    st = R.measure(run, w, t)
    R.out(R.row("uri-build", n, "urllib.parse", "", w, t, st, box[0] == exp,
                f"check={box[0]}; {n} URIs{R.sm('1e6')}; quote(safe='') the parts + urlunsplit", "URIs/s"))
