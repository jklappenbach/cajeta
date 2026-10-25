import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.databind.DeserializationFeature;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.json.JsonMapper;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;

// codec/json-* competitor (bench/codec/json/Json.md): Jackson on the
// nativejson corpus and the JSON_checker set. See competitors/../CONTRACT
// (tokenize = JsonParser.nextToken, bind-skip = empty POJO with unknown
// properties ignored, dom = readTree, serialize = writeValueAsBytes,
// roundtrip = JsonNode.equals, conformance = readTree with
// FAIL_ON_TRAILING_TOKENS).
public class CodecJson {

    /** The bind-skip target: declares no fields, so every field is skipped. */
    public static final class Empty { public Empty() {} }

    static final String[] FILES = { "twitter", "citm_catalog", "canada" };
    static final long[] TOKENS = { 29573, 85035, 223236 };
    static final long[] NODES = { 13914, 37778, 167179 };

    static final JsonFactory FACTORY = new JsonFactory();
    static final ObjectMapper MAPPER = JsonMapper.builder()
        .disable(DeserializationFeature.FAIL_ON_UNKNOWN_PROPERTIES).build();
    static final ObjectMapper STRICT = JsonMapper.builder()
        .enable(DeserializationFeature.FAIL_ON_TRAILING_TOKENS).build();
    static final String VER = com.fasterxml.jackson.databind.cfg.PackageVersion.VERSION.toString();

    static long tokens(byte[] b) throws Exception {
        long n = 0;
        try (JsonParser p = FACTORY.createParser(b)) {
            JsonToken t;
            while ((t = p.nextToken()) != null) {
                n++;
            }
        }
        return n;
    }

    static long nodes(JsonNode n) {
        long c = 1;
        if (n.isContainerNode()) {
            Iterator<JsonNode> it = n.elements();
            while (it.hasNext()) c += nodes(it.next());
        }
        return c;
    }

    public static void main(String[] args) throws Exception {
        String dir = System.getenv("PROFILE_DATA_DIR");
        if (dir == null || dir.isEmpty()) { System.err.println("java/codec/json: PROFILE_DATA_DIR unset"); return; }
        int warmup = BenchRow.envInt("PROFILE_WARMUP", 3);
        int trials = BenchRow.envInt("PROFILE_TRIALS", 10);
        String lib = "jackson-databind";

        for (int f = 0; f < FILES.length; f++) {
            String name = FILES[f];
            Path p = Paths.get(dir, name + ".json");
            if (!Files.exists(p)) {
                for (String slug : new String[] { "json-tokenize", "json-bind-skip", "json-dom", "json-serialize", "json-roundtrip" })
                    BenchRow.skip(slug, name, -1, -1, lib, VER, "corpus file " + name + ".json not fetched (datasets/fetch.sh)", name);
                continue;
            }
            final byte[] b = Files.readAllBytes(p);
            long n = b.length;
            final long expTok = TOKENS[f], expNodes = NODES[f];

            long[] st = BenchRow.measure(() -> tokens(b), warmup, trials);
            long tk = (Long) BenchRow.SINK;
            BenchRow.row("json-tokenize", name, n, n, "jackson-core", VER, warmup, trials, st, tk == expTok,
                "JsonParser.nextToken to end; tokens=" + tk, name, true);

            st = BenchRow.measure(() -> MAPPER.readValue(b, Empty.class), warmup, trials);
            boolean bound = BenchRow.SINK instanceof Empty;
            BenchRow.row("json-bind-skip", name, n, n, lib, VER, warmup, trials, st, bound,
                "readValue into a no-field POJO with FAIL_ON_UNKNOWN_PROPERTIES off; check = document bound without error", name, true);

            st = BenchRow.measure(() -> MAPPER.readTree(b), warmup, trials);
            long nd = nodes((JsonNode) BenchRow.SINK);
            BenchRow.row("json-dom", name, n, n, lib, VER, warmup, trials, st, nd == expNodes,
                "readTree; nodes=" + nd, name, true);

            final JsonNode tree = MAPPER.readTree(b);
            st = BenchRow.measure(() -> MAPPER.writeValueAsBytes(tree), warmup, trials);
            byte[] out = (byte[]) BenchRow.SINK;
            long rn = nodes(MAPPER.readTree(out));
            BenchRow.row("json-serialize", name, n, n, lib, VER, warmup, trials, st, rn == expNodes,
                "writeValueAsBytes of a readTree tree; out=" + out.length + " bytes; reparsed nodes=" + rn, name, true);

            st = BenchRow.measure(() -> {
                JsonNode a = MAPPER.readTree(b);
                JsonNode c = MAPPER.readTree(MAPPER.writeValueAsBytes(a));
                return new JsonNode[] { a, c };
            }, warmup, trials);
            JsonNode[] pair = (JsonNode[]) BenchRow.SINK;
            BenchRow.row("json-roundtrip", name, n, n, lib, VER, warmup, trials, st, pair[0].equals(pair[1]),
                "readTree then writeValueAsBytes then readTree; check JsonNode.equals", name, true);
        }

        // conformance: the 34 JSON_checker files
        List<String> names = new ArrayList<>();
        for (int i = 1; i <= 3; i++) names.add(String.format("pass%02d", i));
        for (int i = 2; i <= 33; i++) if (i != 18) names.add(String.format("fail%02d", i));
        final byte[][] docs = new byte[names.size()][];
        long total = 0;
        for (int i = 0; i < docs.length; i++) {
            Path p = Paths.get(dir, "jsonchecker_" + names.get(i) + ".json");
            if (!Files.exists(p)) {
                BenchRow.skip("json-conformance", "jsonchecker", -1, 34, lib, VER, "jsonchecker files not fetched (datasets/fetch.sh)", "");
                return;
            }
            docs[i] = Files.readAllBytes(p);
            total += docs[i].length;
        }
        long[] st = BenchRow.measure(() -> {
            boolean[] acc = new boolean[docs.length];
            for (int i = 0; i < docs.length; i++) {
                try { STRICT.readTree(docs[i]); acc[i] = true; } catch (Exception e) { acc[i] = false; }
            }
            return acc;
        }, warmup, trials);
        boolean[] acc = (boolean[]) BenchRow.SINK;
        int passOk = 0, failOk = 0;
        StringBuilder mis = new StringBuilder();
        for (int i = 0; i < docs.length; i++) {
            boolean pass = names.get(i).startsWith("pass");
            if (pass == acc[i]) { if (pass) passOk++; else failOk++; }
            else mis.append(' ').append(names.get(i));
        }
        String note = "readTree with FAIL_ON_TRAILING_TOKENS; pass " + passOk + "/3 fail " + failOk + "/31";
        if (mis.length() > 0) note += "; misclassified" + mis;
        BenchRow.row("json-conformance", "jsonchecker", total, 34, lib, VER, warmup, trials, st,
            passOk == 3 && failOk == 31, note, "", false);
    }
}
