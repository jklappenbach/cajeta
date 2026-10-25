// toolchain/debugger-attach debuggee (bench/toolchain/Debugger.md): a running
// JVM, started with a JDWP agent, that calls tick() every millisecond for jdb
// to attach to and break in.
public final class Tick {
    static long tick(long n) {
        return n * 3 + 1;
    }

    public static void main(String[] args) throws InterruptedException {
        System.out.println("ready");
        long acc = 0;
        for (long i = 0;; i++) {
            acc += tick(i);
            Thread.sleep(1);
        }
    }
}
