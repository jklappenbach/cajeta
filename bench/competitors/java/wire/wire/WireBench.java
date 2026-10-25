import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

import org.apache.avro.Schema;
import org.apache.avro.generic.GenericDatumReader;
import org.apache.avro.generic.GenericDatumWriter;
import org.apache.avro.generic.GenericRecord;
import org.apache.avro.io.BinaryDecoder;
import org.apache.avro.io.BinaryEncoder;
import org.apache.avro.io.DecoderFactory;
import org.apache.avro.io.EncoderFactory;

import com.github.luben.zstd.ZstdInputStreamNoFinalizer;
import com.github.luben.zstd.ZstdOutputStreamNoFinalizer;

import benchwire.RecordProto.WireRecord;

// Java wire/ competitors (bench/wire/Encoder.md, SchemaEncoder.md, CompressStream.md):
// protobuf-java 4.36.2 generated WireRecord (encoder-*), Avro 1.12.2 GenericDatum
// over record.avsc (schemaencoder-*), java.util.zip raw Deflater/Inflater and
// zstd-jni streams (compressstream-roundtrip). Record and constants:
// bench/scripts/gen_wire.py (schema/expect.tsv at $WIRE_EXPECT, record.avsc at $WIRE_AVSC).
public final class WireBench {
    static final int POOL = 1024;

    static Map<String, Long> expect() throws Exception {
        Map<String, Long> m = new HashMap<>();
        for (String l : Files.readAllLines(Paths.get(BenchRow.env("WIRE_EXPECT", "expect.tsv")))) {
            if (l.isEmpty() || l.startsWith("#")) continue;
            String[] p = l.split("\t");
            m.put(p[0], Long.parseLong(p[1]));
        }
        return m;
    }

    static byte[] streamData(int size) {
        ByteBuffer b = ByteBuffer.allocate(size + 720).order(ByteOrder.LITTLE_ENDIAN);
        for (int r = 0; b.position() < size; r++) {
            int p = r % POOL;
            for (int k = 1; k <= 100; k++) {
                if (k % 5 == 0) b.putDouble(p * 0.5 + k * 0.25);
                else if (k % 5 == 1) b.putInt((p * 31 + k * 17) % 65536);
                else b.putLong((long) (p + 1) * ((long) k * k * k + 17));
            }
        }
        return Arrays.copyOf(b.array(), size);
    }

    public static void main(String[] args) throws Exception {
        boolean smoke = "1".equals(BenchRow.env("CAJETA_BENCH_SMOKE", "0"));
        String tag = smoke ? "smoke" : "full";
        final int N = smoke ? 100000 : 1000000;
        int warmup = BenchRow.envInt("PROFILE_WARMUP", 2);
        int trials = Math.max(1, BenchRow.envInt("PROFILE_TRIALS", 5));
        Map<String, Long> ex = expect();
        String sz = smoke ? "; smoke size N=1e5 in place of 1e6" : "";
        String note = "; " + BenchRow.FLAGS.replace(",", " ");

        // ---- protobuf-java (generated) ----
        WireRecord[] pool = new WireRecord[POOL];
        for (int r = 0; r < POOL; r++) pool[r] = WireFields.pbFill(r);
        long[] tot = new long[1];
        long[] st = BenchRow.measure(() -> {
            long t = 0;
            for (int i = 0; i < N; i++) {
                byte[] b = pool[i % POOL].toByteArray();
                t += b.length;
            }
            tot[0] = t;
            return t;
        }, warmup, trials);
        BenchRow.row("encoder-encode", N, "protobuf-java", "4.36.2", warmup, trials, st,
            tot[0] == ex.get("pb_total_" + tag), "WireRecord.toByteArray (generated proto3 message); check: total bytes "
            + tot[0] + " == pb_total" + sz, "", "records/s");
        byte[][] enc = new byte[POOL][];
        for (int r = 0; r < POOL; r++) enc[r] = pool[r].toByteArray();
        long[] sum = new long[1];
        st = BenchRow.measure(() -> {
            long s = 0;
            for (int i = 0; i < N; i++) s += WireFields.pbSum(WireRecord.parseFrom(enc[i % POOL]));
            sum[0] = s;
            return s;
        }, warmup, trials);
        BenchRow.row("encoder-decode", N, "protobuf-java", "4.36.2", warmup, trials, st,
            sum[0] == ex.get("decode_sum_" + tag), "WireRecord.parseFrom(byte[]); check: field checksum == decode_sum" + sz,
            "", "records/s");

        // ---- Avro GenericDatum ----
        Schema schema = new Schema.Parser().parse(new String(Files.readAllBytes(Paths.get(BenchRow.env("WIRE_AVSC", "record.avsc")))));
        GenericRecord[] apool = new GenericRecord[POOL];
        for (int r = 0; r < POOL; r++) apool[r] = WireFields.avroFill(schema, r);
        GenericDatumWriter<GenericRecord> w = new GenericDatumWriter<>(schema);
        st = BenchRow.measure(() -> {
            long t = 0;
            ByteArrayOutputStream bo = new ByteArrayOutputStream(1024);
            BinaryEncoder e = null;
            for (int i = 0; i < N; i++) {
                bo.reset();
                e = EncoderFactory.get().directBinaryEncoder(bo, e);
                w.write(apool[i % POOL], e);
                e.flush();
                byte[] b = bo.toByteArray();
                t += b.length;
            }
            tot[0] = t;
            return t;
        }, warmup, trials);
        BenchRow.row("schemaencoder-encode", N, "avro", "1.12.2", warmup, trials, st,
            tot[0] == ex.get("avro_total_" + tag), "GenericDatumWriter over record.avsc (parsed at run time) to a "
            + "directBinaryEncoder, one byte[] per record; check: total bytes " + tot[0] + " == avro_total" + sz, "", "records/s");
        byte[][] aenc = new byte[POOL][];
        for (int r = 0; r < POOL; r++) {
            ByteArrayOutputStream bo = new ByteArrayOutputStream(1024);
            BinaryEncoder e = EncoderFactory.get().directBinaryEncoder(bo, null);
            w.write(apool[r], e);
            e.flush();
            aenc[r] = bo.toByteArray();
        }
        GenericDatumReader<GenericRecord> rd = new GenericDatumReader<>(schema);
        st = BenchRow.measure(() -> {
            long s = 0;
            BinaryDecoder d = null;
            for (int i = 0; i < N; i++) {
                d = DecoderFactory.get().binaryDecoder(aenc[i % POOL], d);
                s += WireFields.avroSum(rd.read(null, d));
            }
            sum[0] = s;
            return s;
        }, warmup, trials);
        BenchRow.row("schemaencoder-decode", N, "avro", "1.12.2", warmup, trials, st,
            sum[0] == ex.get("decode_sum_" + tag), "GenericDatumReader.read(null, binaryDecoder): a fresh GenericRecord per "
            + "record; check: field checksum == decode_sum" + sz, "", "records/s");

        // ---- compressstream-roundtrip ----
        final int SZ = smoke ? (8 << 20) : (64 << 20);
        final int PIECE = 65536, FLUSH = 1 << 20;
        byte[] data = streamData(SZ);
        CRC32 crc = new CRC32();
        crc.update(data);
        boolean crcOk = crc.getValue() == ex.get("stream_crc32_" + tag);
        String ssz = smoke ? "; smoke size 8 MiB in place of 64 MiB" : "";
        byte[][] back = new byte[1][];
        long[] clen = new long[1];
        st = BenchRow.measure(() -> {
            ByteArrayOutputStream comp = new ByteArrayOutputStream(SZ / 2);
            Deflater df = new Deflater(1, true);
            byte[] ob = new byte[65536];
            for (int off = 0; off < SZ; off += PIECE) {
                int n = Math.min(PIECE, SZ - off);
                df.setInput(data, off, n);
                int mode = ((off + n) % FLUSH == 0 && off + n < SZ) ? Deflater.SYNC_FLUSH : Deflater.NO_FLUSH;
                int got;
                do {
                    got = df.deflate(ob, 0, ob.length, mode);
                    comp.write(ob, 0, got);
                } while (got == ob.length || !df.needsInput());
            }
            df.finish();
            while (!df.finished()) comp.write(ob, 0, df.deflate(ob));
            df.end();
            byte[] c = comp.toByteArray();
            clen[0] = c.length;
            Inflater inf = new Inflater(true);
            ByteArrayOutputStream out = new ByteArrayOutputStream(SZ);
            for (int off = 0; off < c.length; off += PIECE) {
                inf.setInput(c, off, Math.min(PIECE, c.length - off));
                int got;
                while ((got = inf.inflate(ob)) > 0) out.write(ob, 0, got);
            }
            inf.end();
            back[0] = out.toByteArray();
            return back[0];
        }, warmup, trials);
        BenchRow.row("compressstream-roundtrip", SZ, "java.util.zip", System.getProperty("java.version"), warmup, trials, st,
            crcOk && Arrays.equals(back[0], data), "Deflater(level 1, nowrap = raw DEFLATE); 64 KiB setInput; SYNC_FLUSH "
            + "every 1 MiB; finish; Inflater(nowrap) in 64 KiB pieces; compressed " + clen[0] + " B; check: byte equality" + ssz,
            "zlib", "MB/s");
        st = BenchRow.measure(() -> {
            ByteArrayOutputStream comp = new ByteArrayOutputStream(SZ / 2);
            ZstdOutputStreamNoFinalizer zo = new ZstdOutputStreamNoFinalizer(comp, 1);
            for (int off = 0; off < SZ; off += PIECE) {
                int n = Math.min(PIECE, SZ - off);
                zo.write(data, off, n);
                if ((off + n) % FLUSH == 0 && off + n < SZ) zo.flush();
            }
            zo.close();
            byte[] c = comp.toByteArray();
            clen[0] = c.length;
            ByteArrayOutputStream out = new ByteArrayOutputStream(SZ);
            try (InputStream zi = new ZstdInputStreamNoFinalizer(new ByteArrayInputStream(c))) {
                byte[] ob = new byte[65536];
                int got;
                while ((got = zi.read(ob)) > 0) out.write(ob, 0, got);
            }
            back[0] = out.toByteArray();
            return back[0];
        }, warmup, trials);
        BenchRow.row("compressstream-roundtrip", SZ, "zstd-jni", "1.5.7-24", warmup, trials, st,
            crcOk && Arrays.equals(back[0], data), "ZstdOutputStreamNoFinalizer(level 1) 64 KiB writes; flush (ZSTD_e_flush) "
            + "every 1 MiB; close; ZstdInputStreamNoFinalizer read in 64 KiB pieces; compressed " + clen[0]
            + " B; check: byte equality" + ssz, "zstd", "MB/s");
    }
}
