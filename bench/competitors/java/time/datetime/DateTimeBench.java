// Java time competitors (benchmark-reference unit 8): the time/ ids the Cajeta
// side runs in bench/src/main/cajeta/bench/time/, same sizes, inputs and check
// constants, with java.time from the JDK. JIT-warmed by the warmup calls; a
// thrown DateTimeException becomes a `failed` row.
import java.lang.management.ManagementFactory;
import java.time.Duration;
import java.time.Instant;
import java.time.LocalDateTime;
import java.time.LocalTime;
import java.time.ZoneId;
import java.time.ZonedDateTime;
import java.time.format.DateTimeFormatter;
import java.time.format.DateTimeFormatterBuilder;
import java.util.Arrays;
import java.util.function.LongPredicate;
import java.util.function.LongSupplier;

public class DateTimeBench {
    static final long N = 10_000_000L, N_ZONE = 1_000_000L, N_FMT = 1_000_000L;
    static final long DURATION_REF = 159_999_997_999_999L;
    static final long LOCALTIME_REF = 432_435_641_193L;
    static final long LOCALDATETIME_REF = 20_264_337_423_627_200L;
    static final long ZONE_REF = -25_266_031_177L;
    static final String FIRST_ISO = "2024-01-01T00:00:00.000000001Z";
    static volatile long sink;
    static volatile Object sinkO;

    static String env(String k, String d) { String v = System.getenv(k); return v == null || v.isEmpty() ? d : v; }

    static long peakRssKb() {
        try {
            for (String l : java.nio.file.Files.readAllLines(java.nio.file.Path.of("/proc/self/status")))
                if (l.startsWith("VmHWM:")) return Long.parseLong(l.replaceAll("[^0-9]", ""));
        } catch (Exception e) { }
        return -1;
    }

    static long allocated() {
        com.sun.management.ThreadMXBean b = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
        return b.getCurrentThreadAllocatedBytes();
    }

    static String runId, ts, ver; static int warmup, trials;

    static void row(String bench, long n, String notes, long[] s, long alloc, String status, boolean ok) {
        long mn = -1, med = -1, mean = -1, p95 = -1;
        if (s != null && s.length > 0) {
            Arrays.sort(s); int k = s.length; long sum = 0; for (long v : s) sum += v;
            mn = s[0]; med = s[k / 2]; mean = sum / k; p95 = s[Math.min(k - 1, k * 95 / 100)];
        }
        double mops = med > 0 ? (double) n / med * 1e3 : 0.0;
        notes = notes.replace(',', ';').replace('\n', ' ').replace('"', '\'');
        System.out.printf("1,%s,%s,%s,time,,%d,,%d,java,%s,java.time,%s,-server,%d,%d,%d,%d,%d,%d,%.2f,Mop/s,%d,-1,%d,-1,-1,%s,%s,%s,%n",
            runId, ts, bench, n, n, ver, Runtime.version(), warmup, trials, mn, med, mean, p95, mops,
            peakRssKb(), alloc, status, ok, notes);
    }

    static void measure(String bench, long n, String notes, LongSupplier f, LongPredicate check) {
        try {
            for (int w = 0; w < warmup; w++) sink = f.getAsLong();
            long[] s = new long[trials]; boolean ok = true;
            for (int t = 0; t < trials; t++) {
                long t0 = System.nanoTime(); long r = f.getAsLong(); s[t] = System.nanoTime() - t0;
                sink = r; ok &= check.test(r);
            }
            long a0 = allocated(); sink = f.getAsLong(); long a = allocated() - a0;
            row(bench, n, notes, s, a, ok ? "ok" : "invalid", ok);
        } catch (RuntimeException e) {
            row(bench, n, e.toString(), null, 0, "failed", false);
        }
    }

    static Instant fmtInstant(long i) { return Instant.ofEpochSecond(1704067200L + i * 997, ((i * 1000003L) % 100000000L) * 10 + 1); }

    static long clockNow() {
        Instant first = Instant.now(), prev = first; long back = 0;
        for (long i = 1; i < N; i++) { Instant t = Instant.now(); if (t.isBefore(prev)) back++; prev = t; }
        sinkO = prev;
        return back == 0 && prev.isAfter(first) ? 1 : 0;
    }

    static long durationArith() {
        Duration three = Duration.ofMillis(3), limit = Duration.ofMillis(10);
        long acc = 0, cnt = 0;
        for (long i = 0; i < N; i++) {
            Duration d = Duration.ofNanos(i).plus(three).multipliedBy(2);
            acc += d.toNanos();
            if (d.compareTo(limit) > 0) cnt++;
        }
        return acc + cnt;
    }

    static long localtimeArith() {
        long acc = 0, cnt = 0;
        for (long i = 0; i < N; i++) {
            LocalTime u = LocalTime.ofSecondOfDay(i % 86400).plusSeconds(18007);
            acc += u.toSecondOfDay();
            if (u.isBefore(LocalTime.NOON)) cnt++;
        }
        return acc + cnt;
    }

    static long localdatetimeArith() {
        LocalDateTime base = LocalDateTime.of(2024, 1, 1, 0, 0);
        long acc = 0;
        for (long i = 0; i < N; i++) {
            LocalDateTime x = base.plusHours(i % 50000);
            acc += x.getYear() * 1_000_000L + x.getMonthValue() * 10_000L + x.getDayOfMonth() * 100L + x.getHour();
        }
        return acc;
    }

    static long zonedRegion() {
        ZoneId la = ZoneId.of("America/Los_Angeles");
        long acc = 0;
        for (long i = 0; i < N_ZONE; i++) {
            ZonedDateTime z = Instant.ofEpochSecond(1577836800L + i * 307).atZone(la);
            acc += z.getOffset().getTotalSeconds() + z.getHour() * 100L + z.getDayOfMonth();
        }
        return acc;
    }

    // ISO-8601 instant with exactly nine fraction digits (ISO_INSTANT trims them).
    static final DateTimeFormatter ISO9 = new DateTimeFormatterBuilder().appendInstant(9).toFormatter();

    static long formatIso() {
        long len = 0; boolean firstOk = false;
        for (long i = 0; i < N_FMT; i++) {
            String s = ISO9.format(fmtInstant(i));
            if (i == 0) firstOk = s.equals(FIRST_ISO);
            len += s.length();
        }
        return firstOk ? len : -1;
    }

    static long parseIso(String[] inp) {
        long good = 0;
        for (int i = 0; i < inp.length; i++) if (Instant.parse(inp[i]).equals(fmtInstant(i))) good++;
        return good;
    }

    public static void main(String[] a) {
        runId = env("PROFILE_RUN_ID", "local"); ts = env("PROFILE_RUN_TS", ""); ver = env("PROFILE_LANG_VERSION", "");
        warmup = Integer.parseInt(env("PROFILE_WARMUP", "3")); trials = Integer.parseInt(env("PROFILE_TRIALS", "10"));
        measure("clock-now", N, "Instant.now (wall clock)", DateTimeBench::clockNow, r -> r == 1);
        measure("duration-arith", N, "Duration", DateTimeBench::durationArith, r -> r == DURATION_REF);
        measure("localtime-arith", N, "LocalTime", DateTimeBench::localtimeArith, r -> r == LOCALTIME_REF);
        measure("localdatetime-arith", N, "LocalDateTime.plusHours", DateTimeBench::localdatetimeArith, r -> r == LOCALDATETIME_REF);
        measure("zoneddatetime-region", N_ZONE, "Instant.atZone(ZoneId)", DateTimeBench::zonedRegion, r -> r == ZONE_REF);
        measure("datetime-format-iso", N_FMT, "DateTimeFormatterBuilder.appendInstant(9)", DateTimeBench::formatIso, r -> r == N_FMT * 30);
        String[] inp = new String[(int) N_FMT];
        for (int i = 0; i < inp.length; i++) inp[i] = ISO9.format(fmtInstant(i));
        measure("datetime-parse-iso", N_FMT, "Instant.parse", () -> parseIso(inp), r -> r == N_FMT);
    }
}
