// io/ competitor, C++ (bench/io/*.md): POSIX read/write/pread/stat,
// iostreams, std::filesystem, std::span, BSD sockets with the peer on a
// std::thread, and Boost.URL. Sizes, inputs, ports and checks are the ones
// competitors/drivers/io/io.sh states (the same as bench/src/main/cajeta/
// bench/io/); every row's notes carry check=<value>, which the competitor
// smoke compares across languages. Usage: io [id ...] (default: every id).
#include "row.h"

#include <arpa/inet.h>
#include <endian.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/url.hpp>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <span>
#include <thread>

namespace fs = std::filesystem;
using row::Stats;

static const int PORT_BASE = 61200;
static std::string g_dir, g_scale, g_dev, g_cxx;
static std::set<std::string> g_only;

static bool want(const char* id) { return g_only.empty() || g_only.count(id); }
static int warm(int d) { return row::smoke() ? 1 : row::env_int("PROFILE_WARMUP", d); }
static int trials(int d) { return row::smoke() ? row::env_int("PROFILE_TRIALS", 3) : row::env_int("PROFILE_TRIALS", d); }
static std::string sm(const char* what) { return row::smoke() ? std::string(" (smoke; the page's ") + what + " is the full run)" : ""; }
static std::string pid_note(int slot) { return "; port " + std::to_string(PORT_BASE + slot) + "; pid " + std::to_string(getpid()); }

static long long seq_bytes() { return row::pick(1073741824LL, 67108864LL); }
static long long line_count() { return row::pick(10000000LL, 1000000LL); }
static std::string P(const std::string& n) { return g_dir + "/" + n; }

// ---------------------------------------------------------------- buffer
static void buffer_rows() {
    const long long N = 16777216;
    if (want("buffer-fill")) {
        long long passes = row::pick(100, 4), acc = 0, exp = 0;
        for (long long p = 0; p < passes; p++) exp += (((p * 7919) % N + p) & 255);
        exp += (N / 256) * 32640;
        int w = warm(2), t = trials(10);
        Stats s = row::measure(w, t, [&] {
            std::vector<uint8_t> d(N);
            long long a = 0;
            for (long long p = 0; p < passes; p++) {
                for (long long i = 0; i < N; i++) d[i] = (uint8_t)((i + p) & 255);
                a += d[(p * 7919) % N];
            }
            for (long long i = 0; i < N; i++) a += d[i];
            acc = a;
        });
        row::emit("buffer-fill", N * passes / 1048576, "std::vector<uint8_t>", g_cxx, w, t, s, acc == exp,
                  "check=" + std::to_string(acc) + "; 16 MiB x " + std::to_string(passes) + " passes" + sm("100 passes"), "MB/s");
    }
    if (want("buffer-read-primitives")) {
        long long reads = row::pick(50000000, 5000000);
        std::vector<uint8_t> d(16777216 + 8);
        for (uint32_t k = 0; k < 2097152; k++) { uint32_t v = (uint32_t)(k * 2654435761u); std::memcpy(&d[k * 4], &v, 4); }
        for (uint64_t k = 0; k < 1048576; k++) { double f = (double)(k & 1023) * 0.5; std::memcpy(&d[8388608 + k * 8], &f, 8); }
        long long ei = 0; double ef = 0;
        for (long long j = 0; j < reads; j++) { ei += (int32_t)(uint32_t)((j & 2097151) * 2654435761u); ef += (double)((j & 1048575) & 1023) * 0.5; }
        long long si = 0; double sf = 0;
        int w = warm(2), t = trials(10);
        Stats s = row::measure(w, t, [&] {
            long long a = 0; double b = 0;
            const uint8_t* p = d.data();
            for (long long j = 0; j < reads; j++) {
                int32_t v; double f;
                std::memcpy(&v, p + (j & 2097151) * 4, 4);
                std::memcpy(&f, p + 8388608 + (j & 1048575) * 8, 8);
                a += v; b += f;
            }
            si = a; sf = b;
        });
        char cb[96]; std::snprintf(cb, sizeof cb, "%lld/%lld", si, (long long)(sf * 2.0));
        row::emit("buffer-read-primitives", reads * 2, "std::vector<uint8_t> + memcpy", g_cxx, w, t, s, si == ei && sf == ef,
                  std::string("check=") + cb + "; " + std::to_string(reads * 2) + " values" + sm("1e8"), "values/s");
    }
    if (want("buffer-slice")) {
        long long views = row::pick(10000000, 1000000), span = N - 64, sum = 0, exp = 0;
        std::vector<uint8_t> d(N);
        for (long long i = 0; i < N; i++) d[i] = (uint8_t)((i * 31 + 7) & 255);
        for (long long j = 0; j < views; j++) exp += (((j * 4099) % span) * 31 + 7) & 255;
        int w = warm(2), t = trials(10);
        Stats s = row::measure(w, t, [&] {
            long long a = 0;
            for (long long j = 0; j < views; j++) {
                std::span<const uint8_t> v(d.data() + (j * 4099) % span, 64);
                a += v[0];
            }
            sum = a;
        });
        row::emit("buffer-slice", views, "std::span", g_cxx, w, t, s, sum == exp,
                  "check=" + std::to_string(sum) + "; " + std::to_string(views) + " views of 64 bytes" + sm("1e7"), "views/s");
    }
}

// ---------------------------------------------------------------- files
static void file_rows() {
    std::string dev = "; device " + g_dev;
    long long mib = seq_bytes() / 1048576;
    std::string seq = P("io-seq-" + g_scale + ".bin");
    if (want("file-read-sequential")) {
        std::vector<char> buf(1048576);
        long long bytes = 0, sum = 0;
        int w = warm(1), t = trials(5);
        Stats s = row::measure(w, t, [&] {
            int fd = open(seq.c_str(), O_RDONLY);
            long long pos = 0, a = 0;
            ssize_t n;
            while ((n = read(fd, buf.data(), buf.size())) > 0) {
                for (long long i = (4096 - (pos & 4095)) & 4095; i < n; i += 4096) a += (uint8_t)buf[i];
                pos += n;
            }
            close(fd);
            bytes = pos; sum = a;
        });
        row::emit("file-read-sequential", mib, "read(2)", g_cxx, w, t, s, bytes == seq_bytes() && sum == mib * 32640,
                  "check=" + std::to_string(bytes) + "/" + std::to_string(sum) + dev + "; " + std::to_string(mib) +
                      " MiB in 1 MiB reads" + sm("1 GiB") + "; page cache warm (no drop without root)", "MB/s");
    }
    if (want("file-write-sequential")) {
        std::vector<char> chunk(1048576);
        for (int j = 0; j < 1048576; j++) chunk[j] = (char)((((j >> 12) * 31) + j) & 255);
        std::string out = P("io-write.bin");
        long long size = 0;
        int w = warm(1), t = trials(5);
        Stats s = row::measure(w, t, [&] {
            int fd = open(out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            for (long long k = 0; k < mib; k++) {
                size_t off = 0;
                while (off < chunk.size()) { ssize_t n = write(fd, chunk.data() + off, chunk.size() - off); if (n <= 0) break; off += n; }
            }
            fsync(fd);
            close(fd);
            struct stat st; stat(out.c_str(), &st); size = st.st_size;
        });
        row::emit("file-write-sequential", mib, "write(2)", g_cxx, w, t, s, size == seq_bytes(),
                  "check=" + std::to_string(size) + dev + "; " + std::to_string(mib) + " MiB in 1 MiB writes + fsync" + sm("1 GiB"), "MB/s");
    }
    if (want("file-random-read")) {
        long long reads = row::pick(100000, 10000), blocks = seq_bytes() / 4096, exp = 0, sum = 0;
        uint64_t x = 42;
        for (long long r = 0; r < reads; r++) { x = x * 6364136223846793005ULL + 1442695040888963407ULL; long long m = (long long)((x >> 33) % (uint64_t)blocks); exp += ((m * 31) & 255) + ((m * 31 + 4095) & 255); }
        std::vector<uint8_t> buf(4096);
        int w = warm(2), t = trials(10);
        Stats s = row::measure(w, t, [&] {
            int fd = open(seq.c_str(), O_RDONLY);
            uint64_t y = 42; long long a = 0;
            for (long long r = 0; r < reads; r++) {
                y = y * 6364136223846793005ULL + 1442695040888963407ULL;
                long long m = (long long)((y >> 33) % (uint64_t)blocks);
                if (pread(fd, buf.data(), 4096, m * 4096) == 4096) a += buf[0] + buf[4095];
            }
            close(fd);
            sum = a;
        });
        row::emit("file-random-read", reads, "pread(2)", g_cxx, w, t, s, sum == exp,
                  "check=" + std::to_string(sum) + dev + "; " + std::to_string(reads) + " reads of 4 KiB over " + std::to_string(mib) + " MiB" + sm("1e5 over 1 GiB"), "reads/s");
    }
    if (want("file-stat")) {
        long long n = row::pick(1000000, 100000), sum = 0;
        std::string sp = P("io-stat.bin");
        int w = warm(2), t = trials(10);
        Stats s = row::measure(w, t, [&] {
            long long a = 0;
            for (long long i = 0; i < n; i++) { struct stat st; if (stat(sp.c_str(), &st) == 0) a += st.st_size; }
            sum = a;
        });
        row::emit("file-stat", n, "stat(2)", g_cxx, w, t, s, sum == n * 4096,
                  "check=" + std::to_string(sum) + dev + "; " + std::to_string(n) + " size queries" + sm("1e6"), "stats/s");
    }
    if (want("filereader-lines")) {
        std::string lp = P("io-lines-" + g_scale + ".txt");
        long long lines = 0;
        int w = warm(1), t = trials(5);
        Stats s = row::measure(w, t, [&] {
            std::ifstream in(lp);
            std::string l; long long c = 0;
            while (std::getline(in, l)) c++;
            lines = c;
        });
        row::emit("filereader-lines", line_count(), "std::getline", g_cxx, w, t, s, lines == line_count(),
                  "check=" + std::to_string(lines) + dev + "; " + std::to_string(line_count()) + " lines of 50 bytes" + sm("1e7"), "lines/s");
    }
    if (want("filewriter-lines")) {
        std::string op = P("io-lines-out.txt");
        long long size = 0, n = line_count();
        int w = warm(1), t = trials(5);
        Stats s = row::measure(w, t, [&] {
            std::ofstream out(op, std::ios::binary | std::ios::trunc);
            char ln[50];
            for (long long k = 0; k < n; k++) {
                long long v = k;
                for (int d = 9; d >= 0; d--) { ln[d] = (char)('0' + v % 10); v /= 10; }
                std::memset(ln + 10, 'a' + (int)(k % 26), 39);
                ln[49] = '\n';
                out.write(ln, 50);
            }
            out.close();
            struct stat st; stat(op.c_str(), &st); size = st.st_size;
        });
        row::emit("filewriter-lines", n, "std::ofstream", g_cxx, w, t, s, size == n * 50,
                  "check=" + std::to_string(size) + dev + "; " + std::to_string(n) + " lines" + sm("1e7"), "lines/s");
    }
    if (want("directory-walk")) {
        std::string root = P("tree-" + g_scale);
        long long dirs = 0, lvl = 1;
        for (int l = 0; l < (row::smoke() ? 2 : 3); l++) { lvl *= 10; dirs += lvl; }
        long long per = dirs + (dirs + 1) * 5, entries = 0;
        int w = warm(2), t = trials(10);
        Stats s = row::measure(w, t, [&] {
            long long c = 0;
            for (int r = 0; r < 10; r++)
                for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) c++;
            entries = c;
        });
        row::emit("directory-walk", per * 10, "std::filesystem::recursive_directory_iterator", g_cxx, w, t, s, entries == per * 10,
                  "check=" + std::to_string(entries) + dev + "; " + std::to_string(per) + " entries x 10" + (row::smoke() ? std::string(" (smoke tree: depth 2; the page's depth-3 tree is the full run)") : ""), "entries/s");
    }
}

// ---------------------------------------------------------------- path
static void path_rows() {
    if (!want("path-join-normalize")) return;
    long long n = row::pick(10000000, 1000000), sum = 0, exp = 0;
    std::vector<std::string> A, B, C;
    for (int k = 0; k < 16; k++) A.push_back("d" + std::to_string(k));
    for (int k = 0; k < 8; k++) B.push_back(k % 2 == 0 ? "../e" + std::to_string(k) : "f" + std::to_string(k) + "/..");
    for (int k = 0; k < 32; k++) C.push_back("file" + std::to_string(k) + ".txt");
    for (long long i = 0; i < n; i++) {
        long long seg = (i & 7) % 2 == 0 ? 2 : ((i & 15) < 10 ? 2 : 3);
        exp += 9 + seg + 1 + ((i & 31) < 10 ? 9 : 10);
    }
    fs::path base("/srv/app");
    int w = warm(2), t = trials(10);
    Stats s = row::measure(w, t, [&] {
        long long a = 0;
        for (long long i = 0; i < n; i++) {
            fs::path p = base / A[i & 15] / B[i & 7] / C[i & 31];
            a += (long long)p.lexically_normal().native().size();
        }
        sum = a;
    });
    row::emit("path-join-normalize", n, "std::filesystem::path", g_cxx, w, t, s, sum == exp,
              "check=" + std::to_string(sum) + "; " + std::to_string(n) + " joins" + sm("1e7") + "; operator/ + lexically_normal", "joins/s");
}

// ---------------------------------------------------------------- bytebuffer
static void bytebuffer_rows() {
    if (!want("bytebuffer-put-get")) return;
    long long pairs = row::pick(25000000, 2500000), sum = 0;
    long long exp = 0;
    for (long long j = 0; j < pairs; j++) exp += (long long)(int32_t)(uint32_t)(j * 2654435761ULL) + (long long)((uint64_t)j * 0x9E3779B97F4A7C15ULL);
    std::vector<uint8_t> buf(65536);
    int w = warm(2), t = trials(10);
    Stats s = row::measure(w, t, [&] {
        uint64_t a = 0;
        for (long long j = 0; j < pairs;) {
            long long end = std::min(pairs, j + 5000);
            uint8_t* wp = buf.data();
            for (long long k = j; k < end; k++) {
                uint32_t v = htobe32((uint32_t)(k * 2654435761ULL));
                uint64_t x = htobe64((uint64_t)k * 0x9E3779B97F4A7C15ULL);
                std::memcpy(wp, &v, 4); std::memcpy(wp + 4, &x, 8); wp += 12;
            }
            const uint8_t* rp = buf.data();
            for (long long k = j; k < end; k++) {
                uint32_t v; uint64_t x;
                std::memcpy(&v, rp, 4); std::memcpy(&x, rp + 4, 8); rp += 12;
                a += (uint64_t)(int64_t)(int32_t)be32toh(v) + be64toh(x);
            }
            j = end;
        }
        sum = (long long)a;
    });
    row::emit("bytebuffer-put-get", pairs * 4, "memcpy + htobe/betoh", g_cxx, w, t, s, sum == exp,
              "check=" + std::to_string(sum) + "; " + std::to_string(pairs) + " int32+int64 pairs put and got (big-endian)" + sm("1e8 ops"), "ops/s");
}

// ---------------------------------------------------------------- net
static sockaddr_in lo(int port) {
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return a;
}
static int listener(int port, int backlog) {
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a = lo(port);
    if (bind(fd, (sockaddr*)&a, sizeof a) != 0 || listen(fd, backlog) != 0) { close(fd); return -1; }
    return fd;
}
static int connect_to(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = lo(port);
    if (connect(fd, (sockaddr*)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}
static bool write_all(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) { ssize_t k = write(fd, c, n); if (k <= 0) return false; c += k; n -= k; }
    return true;
}
static bool read_full(int fd, void* p, size_t n) {
    char* c = (char*)p;
    while (n) { ssize_t k = read(fd, c, n); if (k <= 0) return false; c += k; n -= k; }
    return true;
}

static void net_rows() {
    int one = 1;
    if (want("tcp-echo-latency")) {
        int rounds = (int)row::pick(100000, 1000), port = PORT_BASE + 0;
        std::vector<long long> rtt(rounds);
        long long okc = 0;
        bool sane = true;
        int w = warm(1), t = trials(3);
        Stats s = row::measure(w, t, [&] {
            int l = listener(port, 128);
            if (l < 0) { sane = false; return; }
            std::thread peer([&] {
                int c = accept(l, nullptr, nullptr);
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                char b[65536]; ssize_t n;
                while ((n = read(c, b, sizeof b)) > 0) write_all(c, b, n);
                close(c);
            });
            int c = connect_to(port);
            setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            char msg[64], back[64]; long long ok = 0;
            for (int r = 0; r < rounds; r++) {
                for (int i = 0; i < 64; i++) msg[i] = (char)((r + i) & 255);
                long long t0 = row::now_ns();
                bool g = write_all(c, msg, 64) && read_full(c, back, 64);
                rtt[r] = row::now_ns() - t0;
                if (g && std::memcmp(msg, back, 64) == 0) ok++;
            }
            close(c); peer.join(); close(l);
            okc = ok;
        });
        Stats r = row::stats(rtt);
        row::emit("tcp-echo-latency", rounds, "POSIX sockets", g_cxx, w, t, s, sane && okc == rounds,
                  "check=" + std::to_string(okc) + "; rtt_median_ns=" + std::to_string(r.med) + "; rtt_p95_ns=" + std::to_string(r.p95) +
                      "; " + std::to_string(rounds) + " round trips" + sm("1e5") + pid_note(0) + "; peer on a std::thread (blocking read/write)", "round trips/s");
    }
    if (want("tcp-bulk-throughput")) {
        long long bytes = row::pick(4294967296LL, 268435456LL), got = 0;
        int port = PORT_BASE + 1;
        std::vector<char> chunk(65536);
        for (int i = 0; i < 65536; i++) chunk[i] = (char)(i & 255);
        int w = warm(1), t = trials(5);
        Stats s = row::measure(w, t, [&] {
            int l = listener(port, 128);
            long long rec = 0;
            std::thread peer([&] {
                int c = accept(l, nullptr, nullptr);
                std::vector<char> b(65536); ssize_t n;
                while ((n = read(c, b.data(), b.size())) > 0) rec += n;
                close(c);
            });
            int c = connect_to(port);
            for (long long sent = 0; sent < bytes; sent += 65536) write_all(c, chunk.data(), 65536);
            close(c); peer.join(); close(l);
            got = rec;
        });
        row::emit("tcp-bulk-throughput", bytes / 1048576, "POSIX sockets", g_cxx, w, t, s, got == bytes,
                  "check=" + std::to_string(got) + "; " + std::to_string(bytes / 1048576) + " MiB in 64 KiB writes" + sm("4 GiB") + pid_note(1) + "; peer on a std::thread", "MB/s");
    }
    if (want("tcp-accept-rate")) {
        int n = (int)row::pick(100000, 10000), port = PORT_BASE + 2;
        long long accepted = 0, made = 0;
        int w = warm(1), t = trials(5);
        Stats s = row::measure(w, t, [&] {
            int l = listener(port, 4096);
            std::atomic<long long> mk{0};
            std::thread peer([&] {
                for (int i = 0; i < n; i++) {
                    int c = connect_to(port);
                    if (c < 0) continue;
                    linger lg{1, 0}; setsockopt(c, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
                    close(c); mk++;
                }
            });
            long long a = 0;
            for (int i = 0; i < n; i++) { int c = accept(l, nullptr, nullptr); if (c >= 0) { close(c); a++; } }
            peer.join(); close(l);
            accepted = a; made = mk;
        });
        row::emit("tcp-accept-rate", n, "POSIX sockets", g_cxx, w, t, s, accepted == n && made == n,
                  "check=" + std::to_string(accepted) + "; " + std::to_string(n) + " connections" + sm("1e5") + pid_note(2) + "; connector on a std::thread; client close with SO_LINGER 0", "connections/s");
    }
    if (want("udp-send-rate")) {
        int n = (int)row::pick(1000000, 100000), port = PORT_BASE + 3;
        long long got = 0;
        int w = warm(1), t = trials(5);
        Stats s = row::measure(w, t, [&] {
            int rx = socket(AF_INET, SOCK_DGRAM, 0), tx = socket(AF_INET, SOCK_DGRAM, 0);
            sockaddr_in ra = lo(port), ta = lo(port + 1);
            bind(rx, (sockaddr*)&ra, sizeof ra); bind(tx, (sockaddr*)&ta, sizeof ta);
            long long rec = 0;
            std::thread peer([&] {
                char b[2048], ack = 1;
                while (rec < n) {
                    ssize_t k = recv(rx, b, sizeof b, 0);
                    if (k == 512) { rec++; if (rec % 64 == 0 || rec == n) sendto(rx, &ack, 1, 0, (sockaddr*)&ta, sizeof ta); }
                }
            });
            char d[512] = {0}, ack[8];
            for (int i = 0; i < n;) {
                d[0] = (char)(i & 255);
                sendto(tx, d, 512, 0, (sockaddr*)&ra, sizeof ra);
                i++;
                if (i % 64 == 0 || i == n) recv(tx, ack, sizeof ack, 0);
            }
            peer.join(); close(rx); close(tx);
            got = rec;
        });
        row::emit("udp-send-rate", n, "POSIX sockets", g_cxx, w, t, s, got == n,
                  "check=" + std::to_string(got) + "; " + std::to_string(n) + " datagrams of 512 bytes acked every 64" + sm("1e6") +
                      "; ports " + std::to_string(PORT_BASE + 3) + "-" + std::to_string(PORT_BASE + 4) + "; pid " + std::to_string(getpid()) + "; receiver on a std::thread", "datagrams/s");
    }
}

// ---------------------------------------------------------------- uri
static const char* URIS[16] = {
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
static const char* SEGS[8] = {"report 2026", "caf\xc3\xa9 menu", "a<b>c", "plain", "say \"hi\"", "x{y}z", "100% done", "\xc3\xbc" "ber"};
static const char* VALS[8] = {"alpha", "a<b", "50%", "caf\xc3\xa9", "x\"y", "{id}", "beta-2", "na\xc3\xafve"};

static void uri_rows() {
    std::string ver = std::to_string(BOOST_VERSION / 100000) + "." + std::to_string(BOOST_VERSION / 100 % 1000) + "." + std::to_string(BOOST_VERSION % 100);
    if (want("uri-parse")) {
        long long n = row::pick(1000000, 100000), sum = 0;
        auto cs = [](boost::urls::url_view u) {
            long long c = (long long)u.scheme().size() + (long long)u.encoded_host().size() + u.port_number() +
                          (long long)u.encoded_path().size() + (long long)u.encoded_query().size() + (long long)u.encoded_fragment().size();
            for (auto p : u.params()) c += (long long)p.value.size();
            return c;
        };
        long long per[16], exp = 0;
        for (int k = 0; k < 16; k++) per[k] = cs(boost::urls::parse_uri(URIS[k]).value());
        for (long long i = 0; i < n; i++) exp += per[i & 15];
        bool err = false;
        int w = warm(2), t = trials(10);
        Stats s = row::measure(w, t, [&] {
            long long a = 0;
            for (long long i = 0; i < n; i++) {
                auto r = boost::urls::parse_uri(URIS[i & 15]);
                if (!r) { err = true; continue; }
                a += cs(*r);
            }
            sum = a;
        });
        row::emit("uri-parse", n, "boost::url", ver, w, t, s, !err && sum == exp,
                  "check=" + std::to_string(sum) + "; " + std::to_string(n) + " URIs" + sm("1e6") + "; parse_uri + params()", "URIs/s");
    }
    if (want("uri-build")) {
        long long n = row::pick(1000000, 100000), sum = 0;
        std::string hosts[8];
        for (int k = 0; k < 8; k++) hosts[k] = "api" + std::to_string(k) + ".example.com";
        auto one = [&](long long i) {
            boost::urls::url u;
            u.set_scheme("https");
            u.set_host(hosts[i & 7]);
            u.set_port_number((uint16_t)(8000 + (i & 15)));
            u.set_path(std::string("/v1/") + SEGS[i & 7]);
            u.set_query(std::string("q=") + VALS[(i >> 3) & 7]);
            return (long long)u.buffer().size();
        };
        long long exp = 0, per[64];
        for (int k = 0; k < 64; k++) per[k] = one(k);
        for (long long i = 0; i < n; i++) exp += per[i & 63];
        int w = warm(2), t = trials(10);
        Stats s = row::measure(w, t, [&] {
            long long a = 0;
            for (long long i = 0; i < n; i++) a += one(i);
            sum = a;
        });
        row::emit("uri-build", n, "boost::url", ver, w, t, s, sum == exp,
                  "check=" + std::to_string(sum) + "; " + std::to_string(n) + " URIs" + sm("1e6") + "; set_path/set_query encode", "URIs/s");
    }
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) g_only.insert(argv[i]);
    g_dir = row::env("CAJETA_BENCH_IO_DIR", "build/io-data");
    g_scale = row::smoke() ? "smoke" : "full";
    g_dev = row::env("IO_DEVICE", "unknown");
    g_cxx = row::env("PROFILE_LANG_VERSION", "");
    buffer_rows();
    file_rows();
    path_rows();
    bytebuffer_rows();
    net_rows();
    uri_rows();
    return 0;
}
