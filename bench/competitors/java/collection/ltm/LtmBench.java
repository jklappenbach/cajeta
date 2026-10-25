// Java collection/ltm competitors: ltm-insert and ltm-lookup against RocksDB
// (RocksJava, rocksdbjni 11.8.1) and LMDB (lmdbjava 0.9.3 over LMDB 0.9.33),
// the same workload as LtmInsertBench / LtmLookupBench: N random 16-byte keys
// (16 hex digits of key(i)) with 64-byte values (the key four times), then
// LOOKUPS point gets of key(draw % N); sizes, page budget and directory from
// the CAJETA_BENCH_LTM_* variables the Cajeta side reads (LtmEnv.cajeta).
//   RocksDB: uncompressed, LRU block cache = the budget, write buffer =
//     budget/2, default WriteOptions (WAL on, no per-write fsync), then a
//     waiting flush and close.
//   LMDB: no page budget exists (the file is mmapped and the OS caches it);
//     all N puts in one write transaction, commit, then Env.sync(true).
import java.io.File;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;
import java.util.stream.Stream;
import org.lmdbjava.Dbi;
import org.lmdbjava.DbiFlags;
import org.lmdbjava.Env;
import org.lmdbjava.Txn;
import org.rocksdb.BlockBasedTableConfig;
import org.rocksdb.CompressionType;
import org.rocksdb.FlushOptions;
import org.rocksdb.LRUCache;
import org.rocksdb.Options;
import org.rocksdb.RocksDB;
import org.rocksdb.RocksIterator;

public final class LtmBench {
    static String dir;
    static int n, lookups;
    static long budgetMb;
    static String note, why;

    static void ltmEnv() throws Exception {
        dir = CollGen.env("CAJETA_BENCH_LTM_DIR", "ltm");
        n = (int) CollGen.num("CAJETA_BENCH_LTM_N", 1_000_000);
        lookups = (int) CollGen.num("CAJETA_BENCH_LTM_LOOKUPS", 100_000);
        budgetMb = CollGen.num("CAJETA_BENCH_LTM_BUDGET_MB", 16);
        long minFree = CollGen.num("CAJETA_BENCH_LTM_MIN_FREE_MB", 4096);
        String storage = "unknown";
        File d = new File(dir);
        d.mkdirs();
        if (!d.isDirectory()) why = "disk budget unavailable: cannot create " + dir;
        else {
            String[] f = df("-Pk");
            String[] s = df("--output=source,fstype");
            if (s != null) storage = String.join(" ", s);
            if (f == null || f.length < 4) why = "disk budget unavailable: df -Pk " + dir + " gave no answer";
            else {
                long free = Long.parseLong(f[3]) / 1024;
                if (free < minFree) why = "disk budget unavailable: " + free + " MB free at " + dir + " (" + storage
                    + ") is under CAJETA_BENCH_LTM_MIN_FREE_MB=" + minFree;
            }
        }
        note = "storage=" + storage + " dir=" + dir + " n=" + n + " lookups=" + lookups + " budget=" + budgetMb + "MB (page: 1e7 / 1e6 / 256MB)";
    }

    static String[] df(String opt) throws Exception {
        Process p = new ProcessBuilder("df", opt, dir).redirectErrorStream(true).start();
        String[] lines = new String(p.getInputStream().readAllBytes(), StandardCharsets.UTF_8).split("\n");
        p.waitFor();
        return lines.length > 1 ? lines[1].trim().split("\\s+") : null;
    }

    static byte[] hex16(long v) {
        byte[] b = new byte[16];
        for (int i = 0; i < 16; i++) { int d = (int) ((v >>> (60 - 4 * i)) & 15); b[i] = (byte) (d < 10 ? '0' + d : 'a' + d - 10); }
        return b;
    }

    static void rmrf(Path p) throws Exception {
        if (!Files.exists(p)) return;
        try (Stream<Path> w = Files.walk(p)) { w.sorted(Comparator.reverseOrder()).forEach(x -> x.toFile().delete()); }
    }

    static byte[][] keys, vals, q;

    static Options rocksOpts() {
        long budget = budgetMb << 20;
        return new Options().setCreateIfMissing(true).setCompressionType(CompressionType.NO_COMPRESSION)
            .setWriteBufferSize(budget / 2).setTableFormatConfig(new BlockBasedTableConfig().setBlockCache(new LRUCache(budget)));
    }

    static void rocksFill(Path p) {
        try {
            rmrf(p);
            try (Options o = rocksOpts(); RocksDB db = RocksDB.open(o, p.toString()); FlushOptions fo = new FlushOptions().setWaitForFlush(true)) {
                for (int i = 0; i < n; i++) db.put(keys[i], vals[i]);
                db.flush(fo);
            }
        } catch (Exception e) { throw new RuntimeException(e); }
    }

    static long rocksCount(Path p) {
        try (Options o = new Options(); RocksDB db = RocksDB.openReadOnly(o, p.toString()); RocksIterator it = db.newIterator()) {
            long c = 0;
            for (it.seekToFirst(); it.isValid(); it.next()) c++;
            return c;
        } catch (Exception e) { return -1; }
    }

    static Env<ByteBuffer> lmdbEnv(Path p) throws Exception {
        Files.createDirectories(p);
        long map = (Math.max((long) n * 400, 64L << 20) + 4095) / 4096 * 4096;
        return Env.create().setMapSize(map).setMaxDbs(1).open(p.toFile());
    }

    static long lmdbFill(Path p) {
        try {
            rmrf(p);
            try (Env<ByteBuffer> env = lmdbEnv(p)) {
                Dbi<ByteBuffer> db = env.openDbi((String) null, DbiFlags.MDB_CREATE);
                ByteBuffer kb = ByteBuffer.allocateDirect(16), vb = ByteBuffer.allocateDirect(64);
                try (Txn<ByteBuffer> t = env.txnWrite()) {
                    for (int i = 0; i < n; i++) {
                        kb.clear(); kb.put(keys[i]).flip();
                        vb.clear(); vb.put(vals[i]).flip();
                        db.put(t, kb, vb);
                    }
                    t.commit();
                }
                env.sync(true);
                try (Txn<ByteBuffer> r = env.txnRead()) { return db.stat(r).entries; }
            }
        } catch (Exception e) { throw new RuntimeException(e); }
    }

    public static void main(String[] args) throws Exception {
        ltmEnv();
        String[] libs = {"rocksdbjni", "lmdbjava"};
        if (why != null) {
            for (String lib : libs) {
                if (CollGen.selected("ltm-insert")) CollGen.skip("ltm-insert", lib, n, why);
                if (CollGen.selected("ltm-lookup")) CollGen.skip("ltm-lookup", lib, lookups, why);
            }
            return;
        }
        keys = new byte[n][];
        vals = new byte[n][];
        for (int i = 0; i < n; i++) {
            keys[i] = hex16(CollGen.key(i));
            vals[i] = new byte[64];
            for (int r = 0; r < 4; r++) System.arraycopy(keys[i], 0, vals[i], 16 * r, 16);
        }
        int[] qi = new CollGen().indices(lookups, n);
        q = new byte[lookups][];
        for (int j = 0; j < lookups; j++) q[j] = keys[qi[j]];
        int rc = 0;
        // A store error is a skipped row carrying the message for each of that store's ids, and exit 1.
        try { rocks(); } catch (Exception e) { rc = 1; skipBoth("rocksdbjni", e); }
        try { lmdb(); } catch (Exception e) { rc = 1; skipBoth("lmdbjava", e); }
        System.exit(rc);
    }

    static void skipBoth(String lib, Exception e) {
        String m = "error: " + e;
        if (CollGen.selected("ltm-insert")) CollGen.skip("ltm-insert", lib, n, m);
        if (CollGen.selected("ltm-lookup")) CollGen.skip("ltm-lookup", lib, lookups, m);
    }

    static void rocks() throws Exception {
        RocksDB.loadLibrary();
        Path p = Path.of(dir, "java-rocksdb");
        if (CollGen.selected("ltm-insert"))
            CollGen.row("ltm-insert", "rocksdbjni", "11.8.1", n, CollGen.bench(() -> { rocksFill(p); return p; }, x -> rocksCount(x) == n),
                note + "; uncompressed; block cache = budget; flush at the end");
        if (CollGen.selected("ltm-lookup")) {
            rocksFill(p);
            try (Options o = rocksOpts(); RocksDB db = RocksDB.open(o, p.toString())) {
                CollGen.row("ltm-lookup", "rocksdbjni", "11.8.1", lookups, CollGen.bench(() -> {
                    long s = 0;
                    try { for (byte[] k : q) { byte[] v = db.get(k); if (v != null) s += v.length; } } catch (Exception e) { throw new RuntimeException(e); }
                    return s;
                }, s -> s == 64L * lookups), note + "; get(byte[])");
            }
        }
        rmrf(p);
    }

    static void lmdb() throws Exception {
        Path p = Path.of(dir, "java-lmdb");
        if (CollGen.selected("ltm-insert"))
            CollGen.row("ltm-insert", "lmdbjava", "0.9.3", n, CollGen.bench(() -> lmdbFill(p), c -> c == n),
                note + "; LMDB 0.9.33 has no page budget (mmap); one write txn then sync");
        if (CollGen.selected("ltm-lookup")) {
            lmdbFill(p);
            try (Env<ByteBuffer> env = lmdbEnv(p)) {
                Dbi<ByteBuffer> db = env.openDbi((String) null);
                ByteBuffer kb = ByteBuffer.allocateDirect(16);
                try (Txn<ByteBuffer> r = env.txnRead()) {
                    CollGen.row("ltm-lookup", "lmdbjava", "0.9.3", lookups, CollGen.bench(() -> {
                        long s = 0;
                        for (byte[] k : q) { kb.clear(); kb.put(k).flip(); ByteBuffer v = db.get(r, kb); if (v != null) s += v.remaining(); }
                        return s;
                    }, s -> s == 64L * lookups), note + "; LMDB has no page budget (mmap)");
                }
            }
        }
        rmrf(p);
    }
}
