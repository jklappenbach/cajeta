import io.netty.buffer.ByteBuf;
import io.netty.buffer.PooledByteBufAllocator;

import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.net.URI;
import java.net.URLDecoder;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.DatagramChannel;
import java.nio.channels.FileChannel;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.FileSystems;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardOpenOption;
import java.nio.file.StandardWatchEventKinds;
import java.nio.file.WatchEvent;
import java.nio.file.WatchKey;
import java.nio.file.WatchService;
import java.nio.file.attribute.BasicFileAttributes;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicLong;
import java.util.stream.Stream;

// io/ competitor, Java (bench/io/*.md): ByteBuffer, FileChannel, Files,
// BufferedReader/Writer, Path, Files.walk, WatchService, NIO channels (the
// peer on a thread; the Server rows on one Selector thread), Netty's
// PooledByteBufAllocator and java.net.URI. Sizes, inputs, ports (61600+) and
// checks are those of competitors/drivers/io/io.sh; every row's notes carry
// check=<value>. Usage: java IoBench [id ...].
public final class IoBench {
    static final int PORT_BASE = 61600;
    static Set<String> only = new HashSet<>();
    static String dir, scale, dev;
    static volatile long sink;

    static boolean want(String id) { return only.isEmpty() || only.contains(id); }
    static String p(String n) { return dir + "/" + n; }
    static long seqBytes() { return BenchRow.pick(1073741824L, 67108864L); }
    static long lineCount() { return BenchRow.pick(10000000L, 1000000L); }
    static String pidNote(int slot) { return "; port " + (PORT_BASE + slot) + "; pid " + ProcessHandle.current().pid(); }
    static String sm(String w) { return BenchRow.sm(w); }

    interface Row { void run() throws Exception; }
    static void guard(String slug, String lib, Row r) {
        if (!want(slug)) return;
        try { r.run(); } catch (Throwable e) { BenchRow.err(slug, lib, e); }
    }

    // ---------------------------------------------------------- buffer
    static void bufferRows() {
        final int N = 16777216;
        guard("buffer-fill", "java.nio.ByteBuffer", () -> {
            long passes = BenchRow.pick(100, 4), exp = 0;
            for (long q = 0; q < passes; q++) exp += ((q * 7919) % N + q) & 255;
            exp += (N / 256L) * 32640;
            long[] acc = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                ByteBuffer d = ByteBuffer.allocate(N);
                long a = 0;
                for (long q = 0; q < passes; q++) {
                    for (int i = 0; i < N; i++) d.put(i, (byte) ((i + q) & 255));
                    a += d.get((int) ((q * 7919) % N)) & 255;
                }
                for (int i = 0; i < N; i++) a += d.get(i) & 255;
                acc[0] = a;
            });
            BenchRow.row("buffer-fill", N * passes / 1048576, "java.nio.ByteBuffer", "", w, t, st, acc[0] == exp,
                "check=" + acc[0] + "; 16 MiB x " + passes + " passes" + sm("100 passes"), "MB/s");
        });
        guard("buffer-read-primitives", "java.nio.ByteBuffer", () -> {
            long reads = BenchRow.pick(50000000, 5000000);
            ByteBuffer d = ByteBuffer.allocate(N + 8).order(ByteOrder.LITTLE_ENDIAN);
            for (int k = 0; k < 2097152; k++) d.putInt(k * 4, (int) (k * 2654435761L));
            for (int k = 0; k < 1048576; k++) d.putDouble(8388608 + k * 8, (k & 1023) * 0.5);
            long ei = 0; double ef = 0;
            for (long j = 0; j < reads; j++) { ei += (int) ((j & 2097151) * 2654435761L); ef += ((j & 1048575) & 1023) * 0.5; }
            long[] si = {0}; double[] sf = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long a = 0; double b = 0;
                for (long j = 0; j < reads; j++) {
                    a += d.getInt((int) ((j & 2097151) * 4));
                    b += d.getDouble((int) (8388608 + (j & 1048575) * 8));
                }
                si[0] = a; sf[0] = b;
            });
            BenchRow.row("buffer-read-primitives", reads * 2, "java.nio.ByteBuffer", "", w, t, st, si[0] == ei && sf[0] == ef,
                "check=" + si[0] + "/" + (long) (sf[0] * 2) + "; " + reads * 2 + " values" + sm("1e8") + "; little-endian getInt/getDouble", "values/s");
        });
        guard("buffer-slice", "java.nio.ByteBuffer", () -> {
            long views = BenchRow.pick(10000000, 1000000);
            int span = N - 64;
            ByteBuffer d = ByteBuffer.allocate(N);
            for (int i = 0; i < N; i++) d.put(i, (byte) ((i * 31L + 7) & 255));
            long exp = 0;
            for (long j = 0; j < views; j++) exp += (((j * 4099) % span) * 31 + 7) & 255;
            long[] sum = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long a = 0;
                for (long j = 0; j < views; j++) {
                    ByteBuffer v = d.slice((int) ((j * 4099) % span), 64);
                    a += v.get(0) & 255;
                }
                sum[0] = a;
            });
            BenchRow.row("buffer-slice", views, "java.nio.ByteBuffer", "", w, t, st, sum[0] == exp,
                "check=" + sum[0] + "; " + views + " views of 64 bytes" + sm("1e7") + "; ByteBuffer.slice(index; length)", "views/s");
        });
    }

    // ---------------------------------------------------------- files
    static void fileRows() {
        String dv = "; device " + dev;
        long mib = seqBytes() / 1048576;
        Path seq = Paths.get(p("io-seq-" + scale + ".bin"));
        guard("file-read-sequential", "FileChannel", () -> {
            ByteBuffer buf = ByteBuffer.allocateDirect(1048576);
            long[] r = {0, 0};
            int w = BenchRow.warm(1), t = BenchRow.trials(5);
            long[] st = BenchRow.measure(w, t, () -> {
                try (FileChannel f = FileChannel.open(seq, StandardOpenOption.READ)) {
                    long pos = 0, a = 0;
                    while (true) {
                        buf.clear();
                        int n = f.read(buf);
                        if (n <= 0) break;
                        for (long i = (4096 - (pos & 4095)) & 4095; i < n; i += 4096) a += buf.get((int) i) & 255;
                        pos += n;
                    }
                    r[0] = pos; r[1] = a;
                }
            });
            BenchRow.row("file-read-sequential", mib, "FileChannel", "", w, t, st, r[0] == seqBytes() && r[1] == mib * 32640,
                "check=" + r[0] + "/" + r[1] + dv + "; " + mib + " MiB in 1 MiB reads" + sm("1 GiB") + "; page cache warm (no drop without root)", "MB/s");
        });
        guard("file-write-sequential", "FileChannel", () -> {
            ByteBuffer chunk = ByteBuffer.allocateDirect(1048576);
            for (int j = 0; j < 1048576; j++) chunk.put(j, (byte) ((((j >> 12) * 31) + j) & 255));
            Path out = Paths.get(p("io-write.bin"));
            long[] size = {0};
            int w = BenchRow.warm(1), t = BenchRow.trials(5);
            long[] st = BenchRow.measure(w, t, () -> {
                try (FileChannel f = FileChannel.open(out, StandardOpenOption.WRITE, StandardOpenOption.CREATE, StandardOpenOption.TRUNCATE_EXISTING)) {
                    for (long k = 0; k < mib; k++) {
                        chunk.clear();
                        while (chunk.hasRemaining()) f.write(chunk);
                    }
                    f.force(true);
                }
                size[0] = Files.size(out);
            });
            BenchRow.row("file-write-sequential", mib, "FileChannel", "", w, t, st, size[0] == seqBytes(),
                "check=" + size[0] + dv + "; " + mib + " MiB in 1 MiB writes + force(true)" + sm("1 GiB"), "MB/s");
        });
        guard("file-random-read", "FileChannel", () -> {
            long reads = BenchRow.pick(100000, 10000), blocks = seqBytes() / 4096, exp = 0;
            long x = 42;
            for (long r = 0; r < reads; r++) {
                x = x * 6364136223846793005L + 1442695040888963407L;
                long m = Long.remainderUnsigned(x >>> 33, blocks);
                exp += ((m * 31) & 255) + ((m * 31 + 4095) & 255);
            }
            ByteBuffer buf = ByteBuffer.allocateDirect(4096);
            long[] sum = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                try (FileChannel f = FileChannel.open(seq, StandardOpenOption.READ)) {
                    long y = 42, a = 0;
                    for (long r = 0; r < reads; r++) {
                        y = y * 6364136223846793005L + 1442695040888963407L;
                        long m = Long.remainderUnsigned(y >>> 33, blocks);
                        buf.clear();
                        long pos = m * 4096;
                        while (buf.hasRemaining()) { if (f.read(buf, pos + buf.position()) < 0) break; }
                        a += (buf.get(0) & 255) + (buf.get(4095) & 255);
                    }
                    sum[0] = a;
                }
            });
            BenchRow.row("file-random-read", reads, "FileChannel.read(buf; pos)", "", w, t, st, sum[0] == exp,
                "check=" + sum[0] + dv + "; " + reads + " reads of 4 KiB over " + mib + " MiB" + sm("1e5 over 1 GiB"), "reads/s");
        });
        guard("file-stat", "Files.readAttributes", () -> {
            long n = BenchRow.pick(1000000, 100000);
            Path sp = Paths.get(p("io-stat.bin"));
            long[] sum = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long a = 0;
                for (long i = 0; i < n; i++) a += Files.readAttributes(sp, BasicFileAttributes.class).size();
                sum[0] = a;
            });
            BenchRow.row("file-stat", n, "Files.readAttributes", "", w, t, st, sum[0] == n * 4096,
                "check=" + sum[0] + dv + "; " + n + " size queries" + sm("1e6"), "stats/s");
        });
        guard("filereader-lines", "BufferedReader", () -> {
            Path lp = Paths.get(p("io-lines-" + scale + ".txt"));
            long[] lines = {0};
            int w = BenchRow.warm(1), t = BenchRow.trials(5);
            long[] st = BenchRow.measure(w, t, () -> {
                try (BufferedReader r = Files.newBufferedReader(lp, StandardCharsets.UTF_8)) {
                    long c = 0;
                    while (r.readLine() != null) c++;
                    lines[0] = c;
                }
            });
            BenchRow.row("filereader-lines", lineCount(), "BufferedReader", "", w, t, st, lines[0] == lineCount(),
                "check=" + lines[0] + dv + "; " + lineCount() + " lines of 50 bytes" + sm("1e7") + "; readLine (UTF-8 decode)", "lines/s");
        });
        guard("filewriter-lines", "BufferedWriter", () -> {
            Path op = Paths.get(p("io-lines-out.txt"));
            long n = lineCount();
            long[] size = {0};
            int w = BenchRow.warm(1), t = BenchRow.trials(5);
            long[] st = BenchRow.measure(w, t, () -> {
                try (BufferedWriter out = Files.newBufferedWriter(op, StandardCharsets.UTF_8)) {
                    char[] ln = new char[50];
                    for (long k = 0; k < n; k++) {
                        long v = k;
                        for (int d = 9; d >= 0; d--) { ln[d] = (char) ('0' + v % 10); v /= 10; }
                        Arrays.fill(ln, 10, 49, (char) ('a' + k % 26));
                        ln[49] = '\n';
                        out.write(ln, 0, 50);
                    }
                }
                size[0] = Files.size(op);
            });
            BenchRow.row("filewriter-lines", n, "BufferedWriter", "", w, t, st, size[0] == n * 50,
                "check=" + size[0] + dv + "; " + n + " lines" + sm("1e7") + "; char lines encoded UTF-8", "lines/s");
        });
        guard("directory-walk", "Files.walk", () -> {
            Path root = Paths.get(p("tree-" + scale));
            int depth = BenchRow.smoke() ? 2 : 3;
            long dirs = 0, lvl = 1;
            for (int l = 0; l < depth; l++) { lvl *= 10; dirs += lvl; }
            long per = dirs + (dirs + 1) * 5;
            long[] entries = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long c = 0;
                for (int r = 0; r < 10; r++) {
                    try (Stream<Path> s = Files.walk(root)) { c += s.count() - 1; }
                }
                entries[0] = c;
            });
            BenchRow.row("directory-walk", per * 10, "Files.walk", "", w, t, st, entries[0] == per * 10,
                "check=" + entries[0] + dv + "; " + per + " entries x 10" + (BenchRow.smoke() ? " (smoke tree: depth 2; the page's depth-3 tree is the full run)" : ""), "entries/s");
        });
    }

    // ---------------------------------------------------------- path
    static void pathRows() {
        guard("path-join-normalize", "java.nio.file.Path", () -> {
            long n = BenchRow.pick(10000000, 1000000);
            String[] a = new String[16], b = new String[8], c = new String[32];
            for (int k = 0; k < 16; k++) a[k] = "d" + k;
            for (int k = 0; k < 8; k++) b[k] = k % 2 == 0 ? "../e" + k : "f" + k + "/..";
            for (int k = 0; k < 32; k++) c[k] = "file" + k + ".txt";
            long exp = 0;
            for (long i = 0; i < n; i++) {
                long seg = (i & 7) % 2 == 0 ? 2 : ((i & 15) < 10 ? 2 : 3);
                exp += 9 + seg + 1 + ((i & 31) < 10 ? 9 : 10);
            }
            Path base = Paths.get("/srv/app");
            long[] sum = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long s = 0;
                for (long i = 0; i < n; i++) s += base.resolve(a[(int) (i & 15)]).resolve(b[(int) (i & 7)]).resolve(c[(int) (i & 31)]).normalize().toString().length();
                sum[0] = s;
            });
            BenchRow.row("path-join-normalize", n, "java.nio.file.Path", "", w, t, st, sum[0] == exp,
                "check=" + sum[0] + "; " + n + " joins" + sm("1e7") + "; resolve().normalize()", "joins/s");
        });
    }

    // ---------------------------------------------------------- watcher
    static void watcherRows() {
        guard("watcher-event-latency", "WatchService", () -> {
            int n = (int) BenchRow.pick(1000, 100);
            Path wd = Paths.get(p("watch-java"));
            deleteTree(wd);
            Files.createDirectories(wd);
            long[] lat = new long[n];
            int got = 0;
            try (WatchService ws = FileSystems.getDefault().newWatchService()) {
                wd.register(ws, StandardWatchEventKinds.ENTRY_CREATE);
                for (int k = 0; k < n; k++) {
                    Path target = wd.resolve("f" + k);
                    long t0 = System.nanoTime();
                    Files.createFile(target);
                    long deadline = t0 + 2_000_000_000L;
                    boolean seen = false;
                    while (!seen) {
                        long left = deadline - System.nanoTime();
                        if (left <= 0) break;
                        WatchKey key = ws.poll(left, TimeUnit.NANOSECONDS);
                        if (key == null) break;
                        for (WatchEvent<?> ev : key.pollEvents()) {
                            if (ev.kind() == StandardWatchEventKinds.ENTRY_CREATE && wd.resolve((Path) ev.context()).equals(target)) {
                                if (!seen) lat[got++] = System.nanoTime() - t0;
                                seen = true;
                            }
                        }
                        key.reset();
                    }
                }
            }
            deleteTree(wd);
            long[] st = got > 0 ? BenchRow.statsOf(Arrays.copyOf(lat, got)) : new long[] {0, 0, 0, 0};
            BenchRow.row("watcher-event-latency", n, "WatchService", "", 0, got, st, got == n,
                "check=" + got + "; " + n + " file creations; median create-to-event latency" + sm("1e3") + "; JDK inotify WatchService", "");
        });
    }

    static void deleteTree(Path d) throws IOException {
        if (!Files.exists(d)) return;
        try (Stream<Path> s = Files.walk(d)) {
            for (Path x : s.sorted((u, v) -> v.compareTo(u)).toList()) Files.delete(x);
        }
    }

    // ---------------------------------------------------------- bytebuffer, pool
    static void bytesRows() {
        guard("bytebuffer-put-get", "java.nio.ByteBuffer", () -> {
            long pairs = BenchRow.pick(25000000, 2500000), exp = 0;
            for (long j = 0; j < pairs; j++) exp += (long) (int) (j * 2654435761L) + j * 0x9E3779B97F4A7C15L;
            ByteBuffer b = ByteBuffer.allocate(65536).order(ByteOrder.BIG_ENDIAN);
            long[] sum = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long a = 0;
                for (long j = 0; j < pairs; ) {
                    long end = Math.min(pairs, j + 5000);
                    b.clear();
                    for (long k = j; k < end; k++) { b.putInt((int) (k * 2654435761L)); b.putLong(k * 0x9E3779B97F4A7C15L); }
                    b.flip();
                    for (long k = j; k < end; k++) { a += b.getInt(); a += b.getLong(); }
                    j = end;
                }
                sum[0] = a;
            });
            BenchRow.row("bytebuffer-put-get", pairs * 4, "java.nio.ByteBuffer", "", w, t, st, sum[0] == exp,
                "check=" + sum[0] + "; " + pairs + " int32+int64 pairs put and got (big-endian)" + sm("1e8 ops") + "; putInt/putLong + getInt/getLong", "ops/s");
        });
        guard("bufferpool-acquire-release", "netty PooledByteBufAllocator", () -> {
            long n = BenchRow.pick(10000000, 1000000);
            PooledByteBufAllocator alloc = PooledByteBufAllocator.DEFAULT;
            long[] good = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long g = 0;
                for (long i = 0; i < n; i++) {
                    ByteBuf b = alloc.heapBuffer(65536, 65536);
                    byte x = (byte) (i & 127);
                    b.writeByte(x);
                    if (b.capacity() == 65536 && b.getByte(0) == x) g++;
                    b.release();
                }
                good[0] = g;
            });
            String ver = io.netty.util.Version.identify().getOrDefault("netty-buffer", null) == null ? "" : io.netty.util.Version.identify().get("netty-buffer").artifactVersion();
            BenchRow.row("bufferpool-acquire-release", n, "netty PooledByteBufAllocator", ver, w, t, st, good[0] == n,
                "check=" + good[0] + "; " + n + " acquire/release pairs" + sm("1e7") + "; heapBuffer(65536) + release()", "acquires/s");
        });
    }

    // ---------------------------------------------------------- net (NIO)
    static InetSocketAddress lo(int port) { return new InetSocketAddress("127.0.0.1", port); }
    static void writeAll(SocketChannel c, ByteBuffer b) throws IOException { while (b.hasRemaining()) c.write(b); }
    static boolean readFull(SocketChannel c, ByteBuffer b) throws IOException {
        while (b.hasRemaining()) if (c.read(b) < 0) return false;
        return true;
    }
    static ServerSocketChannel listen(int port, int backlog) throws IOException {
        ServerSocketChannel s = ServerSocketChannel.open();
        s.setOption(StandardSocketOptions.SO_REUSEADDR, true);
        s.bind(lo(port), backlog);
        return s;
    }
    static void joinOrThrow(Thread th, Throwable[] box) throws Exception {
        th.join();
        if (box[0] != null) throw new Exception(box[0]);
    }

    static void netRows() {
        guard("tcp-echo-latency", "NIO SocketChannel", () -> {
            int rounds = (int) BenchRow.pick(100000, 1000), port = PORT_BASE;
            long[] rtt = new long[rounds];
            long[] okc = {0};
            int w = BenchRow.warm(1), t = BenchRow.trials(3);
            long[] st = BenchRow.measure(w, t, () -> {
                try (ServerSocketChannel l = listen(port, 128)) {
                    Throwable[] box = {null};
                    Thread peer = new Thread(() -> {
                        try (SocketChannel c = l.accept()) {
                            c.setOption(StandardSocketOptions.TCP_NODELAY, true);
                            ByteBuffer b = ByteBuffer.allocateDirect(65536);
                            while (true) {
                                b.clear();
                                if (c.read(b) < 0) break;
                                b.flip();
                                writeAll(c, b);
                            }
                        } catch (Throwable e) { box[0] = e; }
                    });
                    peer.start();
                    long ok = 0;
                    try (SocketChannel c = SocketChannel.open(lo(port))) {
                        c.setOption(StandardSocketOptions.TCP_NODELAY, true);
                        ByteBuffer msg = ByteBuffer.allocateDirect(64), back = ByteBuffer.allocateDirect(64);
                        for (int r = 0; r < rounds; r++) {
                            msg.clear();
                            for (int i = 0; i < 64; i++) msg.put(i, (byte) ((r + i) & 255));
                            back.clear();
                            long t0 = System.nanoTime();
                            writeAll(c, msg);
                            boolean full = readFull(c, back);
                            rtt[r] = System.nanoTime() - t0;
                            if (full && back.flip().equals(msg.rewind())) ok++;
                        }
                    }
                    joinOrThrow(peer, box);
                    okc[0] = ok;
                }
            });
            long[] r = BenchRow.statsOf(rtt);
            BenchRow.row("tcp-echo-latency", rounds, "NIO SocketChannel", "", w, t, st, okc[0] == rounds,
                "check=" + okc[0] + "; rtt_median_ns=" + r[1] + "; rtt_p95_ns=" + r[3] + "; " + rounds + " round trips" + sm("1e5") + pidNote(0) + "; peer on a thread (blocking channels)", "round trips/s");
        });
        guard("tcp-bulk-throughput", "NIO SocketChannel", () -> {
            long bytes = BenchRow.pick(4294967296L, 268435456L);
            int port = PORT_BASE + 1;
            ByteBuffer chunk = ByteBuffer.allocateDirect(65536);
            for (int i = 0; i < 65536; i++) chunk.put(i, (byte) (i & 255));
            long[] got = {0};
            int w = BenchRow.warm(1), t = BenchRow.trials(5);
            long[] st = BenchRow.measure(w, t, () -> {
                try (ServerSocketChannel l = listen(port, 128)) {
                    Throwable[] box = {null};
                    long[] rec = {0};
                    Thread peer = new Thread(() -> {
                        try (SocketChannel c = l.accept()) {
                            ByteBuffer b = ByteBuffer.allocateDirect(65536);
                            while (true) { b.clear(); int n = c.read(b); if (n < 0) break; rec[0] += n; }
                        } catch (Throwable e) { box[0] = e; }
                    });
                    peer.start();
                    try (SocketChannel c = SocketChannel.open(lo(port))) {
                        for (long sent = 0; sent < bytes; sent += 65536) { chunk.clear(); writeAll(c, chunk); }
                    }
                    joinOrThrow(peer, box);
                    got[0] = rec[0];
                }
            });
            BenchRow.row("tcp-bulk-throughput", bytes / 1048576, "NIO SocketChannel", "", w, t, st, got[0] == bytes,
                "check=" + got[0] + "; " + bytes / 1048576 + " MiB in 64 KiB writes" + sm("4 GiB") + pidNote(1) + "; peer on a thread", "MB/s");
        });
        guard("tcp-accept-rate", "NIO ServerSocketChannel", () -> {
            int n = (int) BenchRow.pick(100000, 10000), port = PORT_BASE + 2;
            long[] accepted = {0};
            int w = BenchRow.warm(1), t = BenchRow.trials(5);
            long[] st = BenchRow.measure(w, t, () -> {
                try (ServerSocketChannel l = listen(port, 4096)) {
                    Throwable[] box = {null};
                    long[] made = {0};
                    Thread peer = new Thread(() -> {
                        try {
                            for (int i = 0; i < n; i++) {
                                SocketChannel c = SocketChannel.open(lo(port));
                                c.setOption(StandardSocketOptions.SO_LINGER, 0);
                                c.close();
                                made[0]++;
                            }
                        } catch (Throwable e) { box[0] = e; }
                    });
                    peer.start();
                    long a = 0;
                    for (int i = 0; i < n; i++) { l.accept().close(); a++; }
                    joinOrThrow(peer, box);
                    accepted[0] = made[0] == a ? a : 0;
                }
            });
            BenchRow.row("tcp-accept-rate", n, "NIO ServerSocketChannel", "", w, t, st, accepted[0] == n,
                "check=" + accepted[0] + "; " + n + " connections" + sm("1e5") + pidNote(2) + "; connector on a thread; client close with SO_LINGER 0", "connections/s");
        });
        guard("udp-send-rate", "NIO DatagramChannel", () -> {
            int n = (int) BenchRow.pick(1000000, 100000), port = PORT_BASE + 3;
            long[] got = {0};
            int w = BenchRow.warm(1), t = BenchRow.trials(5);
            long[] st = BenchRow.measure(w, t, () -> {
                try (DatagramChannel rx = DatagramChannel.open().bind(lo(port));
                     DatagramChannel tx = DatagramChannel.open().bind(lo(port + 1))) {
                    Throwable[] box = {null};
                    long[] rec = {0};
                    Thread peer = new Thread(() -> {
                        try {
                            ByteBuffer b = ByteBuffer.allocateDirect(2048), ack = ByteBuffer.allocateDirect(1);
                            InetSocketAddress back = lo(port + 1);
                            while (rec[0] < n) {
                                b.clear();
                                rx.receive(b);
                                if (b.position() == 512) {
                                    rec[0]++;
                                    if (rec[0] % 64 == 0 || rec[0] == n) { ack.clear(); rx.send(ack, back); }
                                }
                            }
                        } catch (Throwable e) { box[0] = e; }
                    });
                    peer.start();
                    ByteBuffer d = ByteBuffer.allocateDirect(512), ack = ByteBuffer.allocateDirect(8);
                    InetSocketAddress dest = lo(port);
                    for (int i = 0; i < n; ) {
                        d.clear();
                        d.put(0, (byte) (i & 255));
                        tx.send(d, dest);
                        i++;
                        if (i % 64 == 0 || i == n) { ack.clear(); tx.receive(ack); }
                    }
                    joinOrThrow(peer, box);
                    got[0] = rec[0];
                }
            });
            BenchRow.row("udp-send-rate", n, "NIO DatagramChannel", "", w, t, st, got[0] == n,
                "check=" + got[0] + "; " + n + " datagrams of 512 bytes acked every 64" + sm("1e6") + "; ports " + (PORT_BASE + 3) + "-" + (PORT_BASE + 4) + "; pid " + ProcessHandle.current().pid() + "; receiver on a thread", "datagrams/s");
        });
        guard("server-echo-throughput", "NIO Selector", () -> {
            int clients = 64, port = PORT_BASE + 6;
            long dur = BenchRow.pick(10_000_000_000L, 2_000_000_000L);
            long[] res = {0, 0, 0};
            int w = BenchRow.warm(1), t = BenchRow.trials(3);
            long[] st = BenchRow.measure(w, t, () -> {
                SelectorServer srv = new SelectorServer(port, true);
                srv.start();
                long deadline = System.nanoTime() + dur;
                AtomicLong ok = new AtomicLong(), bad = new AtomicLong();
                Throwable[] box = {null};
                List<Thread> cs = new ArrayList<>();
                for (int id = 0; id < clients; id++) {
                    final int cid = id;
                    Thread th = new Thread(() -> {
                        try (SocketChannel c = SocketChannel.open(lo(port))) {
                            c.setOption(StandardSocketOptions.TCP_NODELAY, true);
                            ByteBuffer msg = ByteBuffer.allocateDirect(1024), back = ByteBuffer.allocateDirect(1024);
                            for (int m = 0; System.nanoTime() < deadline; m++) {
                                msg.clear();
                                for (int i = 0; i < 1024; i++) msg.put(i, (byte) ((i + cid + m) & 255));
                                back.clear();
                                writeAll(c, msg);
                                boolean full = readFull(c, back);
                                if (full && back.flip().equals(msg.rewind())) ok.incrementAndGet(); else bad.incrementAndGet();
                            }
                        } catch (Throwable e) { box[0] = e; }
                    });
                    th.start();
                    cs.add(th);
                }
                for (Thread th : cs) th.join();
                srv.stop();
                if (box[0] != null) throw new Exception(box[0]);
                res[0] = ok.get(); res[1] = bad.get(); res[2] = srv.accepted.get();
            });
            BenchRow.row("server-echo-throughput", res[0], "NIO Selector", "", w, t, st, res[0] > 0 && res[1] == 0 && res[2] == clients,
                "messages=" + res[0] + "; mismatches=" + res[1] + "; connections=" + res[2] + "; " + clients + " clients x " + dur / 1_000_000 + " ms" + sm("10 s") + pidNote(6) + "; one Selector thread serves all; clients on threads in-process", "messages/s");
        });
        guard("server-1k-idle", "NIO Selector", () -> {
            int n = 1000, port = PORT_BASE + 5;
            long hold = BenchRow.pick(10_000_000_000L, 2_000_000_000L);
            long[] res = {0, 0, 0};
            int w = BenchRow.warm(1), t = BenchRow.trials(3);
            long[] st = BenchRow.measure(w, t, () -> {
                SelectorServer srv = new SelectorServer(port, false);
                srv.start();
                List<SocketChannel> cs = new ArrayList<>();
                for (int i = 0; i < n; i++) cs.add(SocketChannel.open(lo(port)));
                long until = System.nanoTime() + 10_000_000_000L;
                while (srv.accepted.get() < n && System.nanoTime() < until) Thread.sleep(1);
                long c0 = BenchRow.cpuMs();
                TimeUnit.NANOSECONDS.sleep(hold / 2);
                long r1 = BenchRow.rssKb();
                TimeUnit.NANOSECONDS.sleep(hold - hold / 2);
                res[2] = BenchRow.cpuMs() - c0; res[1] = r1; res[0] = srv.accepted.get();
                for (SocketChannel c : cs) c.close();
                srv.stop();
            });
            BenchRow.row("server-1k-idle", n, "NIO Selector", "", w, t, st, res[0] == n,
                "check=" + res[0] + "; rss_hold_kb=" + res[1] + "; cpu_ms=" + res[2] + "; " + n + " connections held " + hold / 1_000_000 + " ms" + sm("10 s") + pidNote(5) + "; one Selector thread; clients in-process", "");
        });
    }

    /** A one-thread NIO Selector server: accept, then echo (or just read) every connection. */
    static final class SelectorServer {
        final ServerSocketChannel l;
        final Selector sel;
        final boolean echo;
        final AtomicLong accepted = new AtomicLong();
        volatile boolean running = true;
        Thread th;

        SelectorServer(int port, boolean echo) throws IOException {
            this.l = listen(port, 4096);
            this.l.configureBlocking(false);
            this.sel = Selector.open();
            this.l.register(sel, SelectionKey.OP_ACCEPT);
            this.echo = echo;
        }

        void start() {
            th = new Thread(() -> {
                ByteBuffer b = ByteBuffer.allocateDirect(16384);
                try {
                    while (running) {
                        sel.select(100);
                        for (SelectionKey k : sel.selectedKeys()) {
                            if (!k.isValid()) continue;
                            if (k.isAcceptable()) {
                                SocketChannel c;
                                while ((c = l.accept()) != null) {
                                    c.configureBlocking(false);
                                    c.setOption(StandardSocketOptions.TCP_NODELAY, true);
                                    c.register(sel, SelectionKey.OP_READ);
                                    accepted.incrementAndGet();
                                }
                            } else if (k.isReadable()) {
                                SocketChannel c = (SocketChannel) k.channel();
                                b.clear();
                                int n = c.read(b);
                                if (n < 0) { k.cancel(); c.close(); continue; }
                                if (echo && n > 0) {
                                    b.flip();
                                    // a 1 KiB echo fits the send buffer; spin on the rare short write
                                    while (b.hasRemaining()) c.write(b);
                                }
                            }
                        }
                        sel.selectedKeys().clear();
                    }
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });
            th.start();
        }

        void stop() throws Exception {
            running = false;
            sel.wakeup();
            th.join();
            for (SelectionKey k : sel.keys()) k.channel().close();
            sel.close();
            l.close();
        }
    }

    // ---------------------------------------------------------- uri
    static final String[] URIS = {
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
    };
    static final String[] SEGS = {"report 2026", "café menu", "a<b>c", "plain", "say \"hi\"", "x{y}z", "100% done", "über"};
    static final String[] VALS = {"alpha", "a<b", "50%", "café", "x\"y", "{id}", "beta-2", "naïve"};

    static int utf8Len(String s) { return s.getBytes(StandardCharsets.UTF_8).length; }
    static int len(String s) { return s == null ? 0 : s.length(); }

    static long uriChecksum(URI u) {
        long c = len(u.getScheme()) + len(u.getHost()) + u.getPort() + len(u.getRawPath()) + len(u.getRawQuery()) + len(u.getRawFragment());
        String q = u.getRawQuery();
        if (q != null) {
            for (String pair : q.split("&")) {
                int eq = pair.indexOf('=');
                if (eq >= 0) c += utf8Len(URLDecoder.decode(pair.substring(eq + 1), StandardCharsets.UTF_8));
            }
        }
        return c;
    }

    static void uriRows() {
        guard("uri-parse", "java.net.URI", () -> {
            long n = BenchRow.pick(1000000, 100000);
            long[] per = new long[16];
            for (int k = 0; k < 16; k++) per[k] = uriChecksum(new URI(URIS[k]));
            long exp = 0;
            for (long i = 0; i < n; i++) exp += per[(int) (i & 15)];
            long[] sum = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long a = 0;
                for (long i = 0; i < n; i++) a += uriChecksum(new URI(URIS[(int) (i & 15)]));
                sum[0] = a;
            });
            BenchRow.row("uri-parse", n, "java.net.URI", "", w, t, st, sum[0] == exp,
                "check=" + sum[0] + "; " + n + " URIs" + sm("1e6") + "; new URI + raw components; query values split and URLDecoder.decode (URI has no query parser)", "URIs/s");
        });
        guard("uri-build", "java.net.URI", () -> {
            long n = BenchRow.pick(1000000, 100000);
            String[] hosts = new String[8];
            for (int k = 0; k < 8; k++) hosts[k] = "api" + k + ".example.com";
            long[] per = new long[64];
            for (int k = 0; k < 64; k++) per[k] = buildOne(hosts, k);
            long exp = 0;
            for (long i = 0; i < n; i++) exp += per[(int) (i & 63)];
            long[] sum = {0};
            int w = BenchRow.warm(2), t = BenchRow.trials(10);
            long[] st = BenchRow.measure(w, t, () -> {
                long a = 0;
                for (long i = 0; i < n; i++) a += buildOne(hosts, i);
                sum[0] = a;
            });
            BenchRow.row("uri-build", n, "java.net.URI", "", w, t, st, sum[0] == exp,
                "check=" + sum[0] + "; " + n + " URIs" + sm("1e6") + "; the 7-argument constructor quotes; toASCIIString() encodes non-ASCII", "URIs/s");
        });
    }

    static long buildOne(String[] hosts, long i) throws Exception {
        URI u = new URI("https", null, hosts[(int) (i & 7)], 8000 + (int) (i & 15), "/v1/" + SEGS[(int) (i & 7)], "q=" + VALS[(int) ((i >> 3) & 7)], null);
        return u.toASCIIString().length();
    }

    public static void main(String[] args) {
        only.addAll(Arrays.asList(args));
        dir = BenchRow.env("CAJETA_BENCH_IO_DIR", "build/io-data");
        scale = BenchRow.smoke() ? "smoke" : "full";
        dev = BenchRow.env("IO_DEVICE", "unknown");
        bufferRows();
        fileRows();
        pathRows();
        watcherRows();
        bytesRows();
        netRows();
        uriRows();
    }
}
