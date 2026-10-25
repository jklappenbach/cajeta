// Java "hello-metric" runner — emits one schema-conformant CSV row (columns.txt
// order). Real per-area Java runners (Jackson/fastjson2/DSL-JSON via Maven, with
// JIT warmup) extend this contract in Units 5+.
public class Hello {
    public static void main(String[] args) {
        String runId = System.getenv().getOrDefault("PROFILE_RUN_ID", "local");
        String ts    = System.getenv().getOrDefault("PROFILE_RUN_TS", "");
        String ver   = System.getenv().getOrDefault("PROFILE_LANG_VERSION", "");

        long t0 = System.nanoTime();
        long acc = 0;
        for (long i = 0; i < 1000000L; i++) acc += i;
        long ns = System.nanoTime() - t0;
        if (acc == Long.MIN_VALUE) System.out.print("");

        System.out.printf(
            "1,%s,%s,hello,scaffold,,-1,,1000000,java,%s,jdk,,-XX,0,1,%d,%d,%d,%d,,,-1,-1,-1,-1,-1,ok,true,,%n",
            runId, ts, ver, ns, ns, ns, ns);
    }
}
