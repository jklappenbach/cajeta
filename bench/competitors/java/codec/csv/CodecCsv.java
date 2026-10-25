import com.univocity.parsers.csv.CsvFormat;
import com.univocity.parsers.csv.CsvParser;
import com.univocity.parsers.csv.CsvParserSettings;
import com.univocity.parsers.csv.CsvWriter;
import com.univocity.parsers.csv.CsvWriterSettings;

import java.io.ByteArrayInputStream;
import java.io.InputStreamReader;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.List;

// codec/csv-parse and codec/csv-write competitor (bench/codec/csv/Csv.md):
// univocity-parsers on the generated file (datasets/gen_csv.py), typed rows
// id int64, name String, qty int32, price float64, note String.
public class CodecCsv {

    static final class Row {
        final long id; final String name; final int qty; final double price; final String note;
        Row(long id, String name, int qty, double price, String note) {
            this.id = id; this.name = name; this.qty = qty; this.price = price; this.note = note;
        }
    }

    static CsvParserSettings parserSettings() {
        CsvParserSettings s = new CsvParserSettings();
        s.getFormat().setLineSeparator("\n");
        s.setHeaderExtractionEnabled(true);
        s.setIgnoreLeadingWhitespaces(false);
        s.setIgnoreTrailingWhitespaces(false);
        return s;
    }

    static List<Row> parse(byte[] b) {
        CsvParser p = new CsvParser(parserSettings());
        List<Row> rows = new ArrayList<>();
        p.beginParsing(new InputStreamReader(new ByteArrayInputStream(b), StandardCharsets.UTF_8));
        String[] r;
        while ((r = p.parseNext()) != null) {
            rows.add(new Row(Long.parseLong(r[0]), r[1], Integer.parseInt(r[2]), Double.parseDouble(r[3]), r[4]));
        }
        p.stopParsing();
        return rows;
    }

    static String write(List<Row> rows, int sizeHint) {
        CsvWriterSettings s = new CsvWriterSettings();
        CsvFormat f = s.getFormat();
        f.setLineSeparator("\n");
        s.setIgnoreLeadingWhitespaces(false);
        s.setIgnoreTrailingWhitespaces(false);
        StringWriter out = new StringWriter(sizeHint);
        CsvWriter w = new CsvWriter(out, s);
        w.writeHeaders("id", "name", "qty", "price", "note");
        for (Row r : rows) {
            w.writeRow(Long.toString(r.id), r.name, Integer.toString(r.qty), Double.toString(r.price), r.note);
        }
        w.close();
        return out.toString();
    }

    public static void main(String[] args) throws Exception {
        String dir = System.getenv("PROFILE_DATA_DIR");
        if (dir == null || dir.isEmpty()) { System.err.println("java/codec/csv: PROFILE_DATA_DIR unset"); return; }
        int warmup = BenchRow.envInt("PROFILE_WARMUP", 3);
        int trials = BenchRow.envInt("PROFILE_TRIALS", 10);
        String sm = BenchRow.env("CAJETA_BENCH_SMOKE", "");
        boolean smoke = sm.equals("1") || sm.equals("true") || sm.equals("yes");
        String ds = smoke ? "codec-csv-smoke" : "codec-csv-full";
        String lib = "univocity-parsers";
        String ver = CsvParser.class.getPackage().getImplementationVersion();
        if (ver == null) ver = "2.9.1";
        Path csv = Paths.get(dir, ds + ".csv"), meta = Paths.get(dir, ds + ".meta");
        if (!Files.exists(csv) || !Files.exists(meta)) {
            String why = ds + ".csv not generated (datasets/gen_csv.py)";
            BenchRow.skip("csv-parse", ds, -1, -1, lib, ver, why, "");
            BenchRow.skip("csv-write", ds, -1, -1, lib, ver, why, "");
            return;
        }
        String[] m = Files.readString(meta).trim().split("\\s+");
        long expRows = Long.parseLong(m[0]), expQty = Long.parseLong(m[1]), expBytes = Long.parseLong(m[2]);
        final byte[] b = Files.readAllBytes(csv);
        long n = b.length;
        String extra = smoke ? "; smoke input 4 MB" : "";

        long[] st = BenchRow.measure(() -> parse(b), warmup, trials);
        @SuppressWarnings("unchecked") List<Row> rows = (List<Row>) BenchRow.SINK;
        long q = 0;
        for (Row r : rows) q += r.qty;
        BenchRow.row("csv-parse", ds, n, n, lib, ver, warmup, trials, st, rows.size() == expRows && q == expQty,
            "CsvParser.parseNext + Long/Integer/Double.parse per field; rows=" + rows.size() + " qty_sum=" + q + extra, "", true);

        final List<Row> typed = parse(b);
        final int hint = (int) n + 64;
        st = BenchRow.measure(() -> write(typed, hint), warmup, trials);
        long outBytes = ((String) BenchRow.SINK).getBytes(StandardCharsets.UTF_8).length;
        BenchRow.row("csv-write", ds, n, n, lib, ver, warmup, trials, st, outBytes == expBytes,
            "CsvWriter to a StringWriter; Double.toString prices; out=" + outBytes + " bytes" + extra, "", true);
    }
}
