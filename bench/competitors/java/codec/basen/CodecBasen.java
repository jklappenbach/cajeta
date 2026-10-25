import org.apache.commons.codec.binary.Base32;

import java.util.Arrays;
import java.util.Base64;

// codec/base64-* and codec/base32-* competitor (bench/codec/Base64.md,
// Base32.md): java.util.Base64 and Apache commons-codec Base32 on 1 MiB of
// data[i] = (i*31+7) & 0xFF.
public class CodecBasen {
    public static void main(String[] args) throws Exception {
        int warmup = BenchRow.envInt("PROFILE_WARMUP", 3);
        int trials = BenchRow.envInt("PROFILE_TRIALS", 1000);
        final int n = 1048576;
        final byte[] data = new byte[n];
        for (int i = 0; i < n; i++) data[i] = (byte) ((i * 31 + 7) & 0xFF);
        String jdk = System.getProperty("java.version");

        final Base64.Encoder enc = Base64.getEncoder();
        final Base64.Decoder dec = Base64.getDecoder();
        long[] st = BenchRow.measure(() -> enc.encode(data), warmup, trials);
        int len = ((byte[]) BenchRow.SINK).length;
        BenchRow.row("base64-encode", "", n, n, "java.util.Base64", jdk, warmup, trials, st, len == 1398104,
            "Base64.getEncoder().encode(byte[]); length=" + len, "", true);
        final byte[] e64 = enc.encode(data);
        st = BenchRow.measure(() -> dec.decode(e64), warmup, trials);
        boolean eq = Arrays.equals((byte[]) BenchRow.SINK, data);
        BenchRow.row("base64-decode", "", n, n, "java.util.Base64", jdk, warmup, trials, st, eq,
            "Base64.getDecoder().decode(byte[]); check equality", "", true);

        final Base32 b32 = new Base32();
        String cv = Base32.class.getPackage().getImplementationVersion();
        if (cv == null) cv = "1.23.0";
        st = BenchRow.measure(() -> b32.encode(data), warmup, trials);
        len = ((byte[]) BenchRow.SINK).length;
        BenchRow.row("base32-encode", "", n, n, "commons-codec", cv, warmup, trials, st, len == 1677728,
            "new Base32().encode(byte[]); length=" + len, "", true);
        final byte[] e32 = b32.encode(data);
        st = BenchRow.measure(() -> b32.decode(e32), warmup, trials);
        eq = Arrays.equals((byte[]) BenchRow.SINK, data);
        BenchRow.row("base32-decode", "", n, n, "commons-codec", cv, warmup, trials, st, eq,
            "new Base32().decode(byte[]); check equality", "", true);
    }
}
