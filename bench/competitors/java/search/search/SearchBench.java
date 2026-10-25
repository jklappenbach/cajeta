import java.lang.management.ManagementFactory;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.*;

import org.apache.commons.text.similarity.JaroWinklerSimilarity;
import org.apache.commons.text.similarity.LevenshteinDistance;
import org.apache.lucene.analysis.Analyzer;
import org.apache.lucene.analysis.LowerCaseFilter;
import org.apache.lucene.analysis.Tokenizer;
import org.apache.lucene.analysis.ngram.NGramTokenizer;
import org.apache.lucene.document.Document;
import org.apache.lucene.document.Field;
import org.apache.lucene.document.StringField;
import org.apache.lucene.document.TextField;
import org.apache.lucene.index.*;
import org.apache.lucene.search.*;
import org.apache.lucene.store.ByteBuffersDirectory;
import org.apache.lucene.store.Directory;
import org.apache.lucene.util.BytesRef;

// search/ competitors in Java (bench/search/*.md): Apache commons-text for the
// distances, Lucene FuzzyQuery for fuzzy top-k, a Lucene NGramTokenizer index
// for ngram build and query. Inputs from the shared description
// (bench/scripts/search_ref.py: LCG31, seeds 12345/777/999); constants are
// search_ref.py's. v1 rows, area search.
public final class SearchBench {
    static String env(String k, String d) { String v = System.getenv(k); return (v == null || v.isEmpty()) ? d : v; }
    static int envInt(String k, int d) { try { return Integer.parseInt(env(k, "" + d)); } catch (NumberFormatException e) { return d; } }
    static final boolean SMOKE = env("CAJETA_BENCH_SMOKE", "").equals("1");
    static final int W = envInt("PROFILE_WARMUP", 1), T = Math.max(1, envInt("PROFILE_TRIALS", 5));
    static final String LANG_VER = clean(env("PROFILE_LANG_VERSION", "java " + System.getProperty("java.version")));
    static final String FLAGS = clean(env("PROFILE_JAVA_FLAGS", "java default flags") + " (C2 JIT warmed)");
    static final String CT = "1.15.0", LU = org.apache.lucene.util.Version.LATEST.toString();
    static volatile Object SINK;

    static String clean(String s) { return s.replace(",", ";").replace("\"", ""); }
    static long peakRssKb() {
        try { for (String l : Files.readAllLines(Paths.get("/proc/self/status"))) if (l.startsWith("VmHWM:")) return Long.parseLong(l.split("\\s+")[1]); }
        catch (Exception e) { }
        return -1;
    }
    interface Body { Object run() throws Exception; }
    static Object last;
    static long[] measure(Body b) throws Exception {
        for (int i = 0; i < W; i++) SINK = b.run();
        long[] s = new long[T];
        for (int i = 0; i < T; i++) { long t0 = System.nanoTime(); Object r = b.run(); s[i] = System.nanoTime() - t0; SINK = r; last = r; }
        Arrays.sort(s);
        long sum = 0; for (long x : s) sum += x;
        int n = s.length;
        return new long[] { s[0], s[n / 2], sum / n, s[Math.min(n - 1, n * 95 / 100)] };
    }
    static void row(String slug, long input, String lib, String ver, long[] st, boolean ok, String notes, String unit) {
        String thr = st[1] > 0 ? String.format("%.1f", input / (double) st[1] * 1e9) : "";
        System.out.println(String.join(",", "1", env("PROFILE_RUN_ID", "local"), env("PROFILE_RUN_TS", ""), slug, "search", "", "0", "",
            "" + input, "java", LANG_VER, lib, ver, FLAGS, "" + W, "" + T, "" + st[0], "" + st[1], "" + st[2], "" + st[3], thr, unit,
            "" + peakRssKb(), "-1", "-1", "-1", "-1", ok ? "ok" : "invalid", ok ? "true" : "false", clean(notes), ""));
    }

    static long x;
    static int rnd(int m) { x = (x * 1103515245L + 12345L) & 0x7fffffffL; return (int) (((x >> 15) * m) >> 16); }
    static String rstr(int n, int alpha) { char[] c = new char[n]; for (int i = 0; i < n; i++) c[i] = (char) ('a' + rnd(alpha)); return new String(c); }
    static String[] keys(int k) { x = 777; String[] o = new String[k]; for (int i = 0; i < k; i++) { int n = 8 + rnd(9); o[i] = rstr(n, 26); } return o; }
    static String[] queries(String[] ks, int q) {
        x = 999;
        String[] o = new String[q];
        for (int i = 0; i < q; i++) {
            char[] key = ks[rnd(ks.length)].toCharArray();
            int op = rnd(3), n = key.length;
            if (op == 0) { int p = rnd(n); char c = (char) ('a' + rnd(26)); key[p] = c; o[i] = new String(key); }
            else if (op == 1) { int p = rnd(n - 1); char t = key[p]; key[p] = key[p + 1]; key[p + 1] = t; o[i] = new String(key); }
            else { int p = rnd(n); String s = new String(key); o[i] = s.substring(0, p) + s.substring(p + 1); }
        }
        return o;
    }
    static List<String> grams(String s) {
        s = s.toLowerCase(Locale.ROOT);
        List<String> g = new ArrayList<>();
        if (s.isEmpty()) return g;
        if (s.length() < 3) { g.add(s); return g; }
        for (int i = 0; i + 3 <= s.length(); i++) g.add(s.substring(i, i + 3));
        return g;
    }
    static int osa(String a, String b) {
        int n = a.length(), m = b.length();
        if (n == 0) return m;
        if (m == 0) return n;
        int[] pp = new int[m + 1], p = new int[m + 1], c = new int[m + 1];
        for (int j = 0; j <= m; j++) p[j] = j;
        for (int i = 1; i <= n; i++) {
            c[0] = i;
            for (int k = 1; k <= m; k++) {
                int cost = a.charAt(i - 1) == b.charAt(k - 1) ? 0 : 1;
                int best = Math.min(Math.min(p[k] + 1, c[k - 1] + 1), p[k - 1] + cost);
                if (i > 1 && k > 1 && a.charAt(i - 1) == b.charAt(k - 2) && a.charAt(i - 2) == b.charAt(k - 1)) best = Math.min(best, pp[k - 2] + 1);
                c[k] = best;
            }
            int[] t = pp; pp = p; p = c; c = t;
        }
        return p[m];
    }

    static final class NGram3 extends Analyzer {
        @Override protected TokenStreamComponents createComponents(String f) {
            Tokenizer t = new NGramTokenizer(3, 3);
            return new TokenStreamComponents(t, new LowerCaseFilter(t));
        }
    }
    static Directory buildNgram(String[] ks) throws Exception {
        Directory d = new ByteBuffersDirectory();
        try (IndexWriter w = new IndexWriter(d, new IndexWriterConfig(new NGram3()).setOpenMode(IndexWriterConfig.OpenMode.CREATE))) {
            for (String k : ks) { Document doc = new Document(); doc.add(new TextField("g", k, Field.Store.NO)); w.addDocument(doc); }
        }
        return d;
    }

    public static void main(String[] args) throws Exception {
        String sn;
        // ---- distance (commons-text) ----
        int m = SMOKE ? 100000 : 1000000;
        sn = SMOKE ? "; smoke size (" + m + " pairs in place of 1e6)" : "";
        x = 12345;
        String[] a = new String[m], b = new String[m];
        for (int i = 0; i < m; i++) { a[i] = rstr(16, 8); b[i] = rstr(16, 8); }
        LevenshteinDistance lev = LevenshteinDistance.getDefaultInstance();
        long[] st = measure(() -> { long s = 0; for (int i = 0; i < m; i++) s += lev.apply(a[i], b[i]); return s; });
        long ls = (Long) last, lwant = SMOKE ? 1276356L : 12760743L;
        row("distance-levenshtein", m, "commons-text", CT, st, ls == lwant, "LevenshteinDistance.getDefaultInstance().apply per pair; sum " + ls + " == " + lwant + sn, "pairs/s");
        JaroWinklerSimilarity jw = new JaroWinklerSimilarity();
        st = measure(() -> { double s = 0; for (int i = 0; i < m; i++) s += jw.apply(a[i], b[i]); return s; });
        // commons-text's own definition differs from the shared one (half transpositions as a real t/2, not
        // integer t/2; the Winkler boost at jaro >= 0.7, not > 0.7), so the row checks against the sum under
        // that definition (search_ref.py's pairs, computed by a Python transcription of commons-text 1.15.0)
        // and notes the shared-definition sum it would otherwise be compared with.
        double js = (Double) last, jwant = SMOKE ? 60446.71013338285 : 604336.3172430887;
        double shared = SMOKE ? 61342.531185943684 : 613325.3143160543;
        row("distance-jaro-winkler", m, "commons-text", CT, st, Math.abs(js - jwant) <= 1e-6 * m,
            String.format(Locale.ROOT, "JaroWinklerSimilarity.apply per pair; sum %.6f within 1e-6*M of %s = commons-text's own definition "
                + "(real-valued half transpositions; boost at jaro >= 0.7); the shared definition (integer half transpositions; strsim/rapidfuzz) sums to %s", js, jwant, shared) + sn, "pairs/s");

        // ---- fuzzy top-k (Lucene FuzzyQuery) ----
        int k = SMOKE ? 10000 : 100000, q = SMOKE ? 1000 : 10000;
        String[] ks = keys(k);
        String[] qs = queries(ks, q);
        Directory kd = new ByteBuffersDirectory();
        try (IndexWriter w = new IndexWriter(kd, new IndexWriterConfig())) {
            for (String s : ks) { Document doc = new Document(); doc.add(new StringField("k", s, Field.Store.YES)); w.addDocument(doc); }
            w.forceMerge(1);
        }
        DirectoryReader kr = DirectoryReader.open(kd);
        IndexSearcher ksr = new IndexSearcher(kr);
        StoredFields sf = kr.storedFields();
        st = measure(() -> {
            TopDocs[] r = new TopDocs[q];
            for (int i = 0; i < q; i++) r[i] = ksr.search(new FuzzyQuery(new Term("k", qs[i]), 2, 0, 50, true), 10);
            return r;
        });
        TopDocs[] res = (TopDocs[]) last;
        long hits = 0, truthN = 0, total = 0;
        for (TopDocs td : res) total += td.scoreDocs.length;
        for (int i = 0; i < 100; i++) {
            String s = qs[i];
            int t = Math.min(Math.max(1, s.length() / 2), 2);
            List<Object[]> truth = new ArrayList<>();
            for (String kk : ks) { int d = osa(s, kk); if (d <= t) truth.add(new Object[] { d, kk }); }
            truth.sort((u, v) -> (Integer) u[0] != (int) (Integer) v[0] ? Integer.compare((Integer) u[0], (Integer) v[0]) : ((String) u[1]).compareTo((String) v[1]));
            if (truth.size() > 10) truth = truth.subList(0, 10);
            truthN += truth.size();
            Set<String> tset = new HashSet<>();
            for (Object[] o : truth) tset.add((String) o[1]);
            int lastD = truth.isEmpty() ? -1 : (Integer) truth.get(truth.size() - 1)[0];
            int h = 0;
            for (ScoreDoc sd : res[i].scoreDocs) {
                String kk = sf.document(sd.doc).get("k");
                if (tset.contains(kk) || (truth.size() == 10 && osa(s, kk) == lastD)) h++;
            }
            hits += Math.min(h, truth.size());
        }
        double recall = truthN > 0 ? hits / (double) truthN : 0;
        row("fuzzy-topk", q, "lucene-core", LU, st, recall >= 0.99,
            String.format(Locale.ROOT, "FuzzyQuery(term; maxEdits 2; prefix 0; maxExpansions 50; transpositions) top 10 over %d keyword docs; "
                + "recall@10 %.4f vs brute-force OSA <= min(t;2) over the first 100 queries (truth hits %d; maxEdits 2 is Lucene's cap where the page's t = len/2; "
                + "ties at rank 10 count as hits); %d results", k, recall, truthN, total)
                + (SMOKE ? "; smoke size (" + q + " queries over " + k + " keys in place of 1e4 over 1e5)" : ""), "queries/s");
        kr.close();

        // ---- ngram build + query (Lucene NGramTokenizer 3..3) ----
        st = measure(() -> buildNgram(ks));
        Directory nd = (Directory) last;
        DirectoryReader nr = DirectoryReader.open(nd);
        long post = 0;
        for (LeafReaderContext lc : nr.leaves()) {
            Terms terms = lc.reader().terms("g");
            if (terms == null) continue;
            TermsEnum te = terms.iterator();
            while (te.next() != null) post += te.docFreq();
        }
        long pwant = SMOKE ? 100180L : 998465L;
        row("ngram-build", k, "lucene-analysis-common", LU, st, post == pwant,
            "IndexWriter over ByteBuffersDirectory; NGramTokenizer(3;3)+LowerCaseFilter; one doc per key; distinct postings (sum of docFreq; Lucene keeps one posting per term per doc) "
                + post + " == " + pwant + (SMOKE ? "; smoke size (" + k + " keys in place of 1e5)" : ""), "keys/s");
        int nq = SMOKE ? 10000 : 100000;
        String[] qq = queries(ks, nq);
        IndexSearcher ns = new IndexSearcher(nr);
        ns.setQueryCache(null);
        st = measure(() -> {
            long c = 0;
            for (String s : qq) {
                BooleanQuery.Builder bq = new BooleanQuery.Builder();
                for (String g : new LinkedHashSet<>(grams(s))) bq.add(new TermQuery(new Term("g", g)), BooleanClause.Occur.SHOULD);
                c += ns.count(bq.build());
            }
            return c;
        });
        long c = (Long) last, cwant = SMOKE ? 545614L : 53088630L;
        row("ngram-query", nq, "lucene-core", LU, st, c == cwant,
            "BooleanQuery of SHOULD TermQuery per distinct query trigram; IndexSearcher.count (query cache off); candidates " + c + " == " + cwant
                + (SMOKE ? "; smoke size (" + nq + " queries over " + k + " keys in place of 1e5 over 1e5)" : ""), "queries/s");
        nr.close();
    }
}
