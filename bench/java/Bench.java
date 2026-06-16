import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonParser;
import com.google.gson.stream.JsonReader;
import com.google.gson.stream.JsonToken;

import java.io.ByteArrayInputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Paths;

// Streaming-tokenization throughput for Jackson and Gson, mirroring the Cajeta
// JsonReader.next() loop: pull every token to end-of-document, count them, no
// DOM, numbers read lazily (as raw text). Bytes are read once and parsed N times
// against a warmed-up JIT. Same machine + same files as the Cajeta benchmark.
public class Bench {

    static final JsonFactory JACKSON = new JsonFactory();

    static long jackson(byte[] b) throws Exception {
        JsonParser p = JACKSON.createParser(b);
        long n = 0;
        while (p.nextToken() != null) {
            n++;
        }
        p.close();
        return n;
    }

    static long gson(byte[] b) throws Exception {
        JsonReader r = new JsonReader(new InputStreamReader(new ByteArrayInputStream(b), StandardCharsets.UTF_8));
        r.setLenient(true);
        long n = 0;
        while (true) {
            JsonToken t = r.peek();
            switch (t) {
                case BEGIN_OBJECT: r.beginObject(); n++; break;
                case END_OBJECT:   r.endObject();   n++; break;
                case BEGIN_ARRAY:  r.beginArray();  n++; break;
                case END_ARRAY:    r.endArray();    n++; break;
                case NAME:         r.nextName();    n++; break;
                case STRING:       r.nextString();  n++; break;
                case NUMBER:       r.nextString();  n++; break; // raw, lazy
                case BOOLEAN:      r.nextBoolean(); n++; break;
                case NULL:         r.nextNull();    n++; break;
                case END_DOCUMENT: r.close(); return n;
                default:           r.close(); return n;
            }
        }
    }

    static void bench(String name, byte[] b, int warmup, int iters) throws Exception {
        long jt = 0, gt = 0;
        for (int i = 0; i < warmup; i++) { jt = jackson(b); gt = gson(b); }

        long t0 = System.nanoTime();
        for (int i = 0; i < iters; i++) { jt = jackson(b); }
        long jns = System.nanoTime() - t0;

        long g0 = System.nanoTime();
        for (int i = 0; i < iters; i++) { gt = gson(b); }
        long gns = System.nanoTime() - g0;

        double mb = (double) b.length * iters / 1_000_000.0;
        double jmbps = mb / (jns / 1e9);
        double gmbps = mb / (gns / 1e9);
        double jms = (jns / 1e6) / (double) iters;
        double gms = (gns / 1e6) / (double) iters;
        System.out.printf("%-13s %8d B  Jackson: %7.1f ms/parse-batch? %6.2f ms  %7.1f MB/s (%d tok)   Gson: %6.2f ms  %7.1f MB/s (%d tok)%n",
                name, b.length, 0.0, jms, jmbps, jt, gms, gmbps, gt);
    }

    public static void main(String[] args) throws Exception {
        System.out.println("== Java JSON streaming-tokenize throughput (Jackson " +
                JsonParser.class.getPackage().getImplementationVersion() + ", Gson, JIT warmed) ==");
        String dir = "/tmp/jsonbench/";
        bench("twitter",      Files.readAllBytes(Paths.get(dir + "twitter.json")),      100, 200);
        bench("citm_catalog", Files.readAllBytes(Paths.get(dir + "citm_catalog.json")), 100, 200);
        bench("canada",       Files.readAllBytes(Paths.get(dir + "canada.json")),       100, 200);
    }
}
