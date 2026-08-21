//
// cajeta-llama plan 15.1.15 — the live-allocation set must survive a real
// model load.
//
// Binding Llama-3.1-8B Q4_K_M prints:
//   "live-allocation set reached load cap (196608 / 262144). Subsequent
//    allocations won't be tracked; field auto-drop may double-free aliased
//    addresses past this point."
//
// The set is a fixed 256K-slot open-addressed table that stops tracking at
// 75% load. Two consequences are documented and they CONTRADICT each other:
// the header comment calls the overflow "correctness-preserving", while the
// warning the runtime actually prints says it may double-free. Both cannot
// hold. These tests pin the observable behaviour rather than either claim.
//
// Everything before this ran on toy fixtures far under the cap, which is why
// it took a real model to surface.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Above CAJETA_LIVE_SET_LOAD_CAP (196608) so the table must grow to stay
// accurate, but small enough to stay cheap (~220k * 24B of instances).
constexpr int kOverCap = 220000;

int64_t runI64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn ? fn() : -1;
}

std::string program(const std::string& tail) {
    return
        "package test;\n"
        "import cajeta.lang.Cajeta;\n"
        "public class N { public int32 v; }\n"
        "public final class D {\n"
        // The owner is a LOCAL of a helper, so it drops at helper exit. A
        // plain `a = null` in the caller would not drop it -- locals have no
        // pre-overwrite drop -- and would measure nothing.
        "    static void once(int32 n) {\n"
        "        N[] a #= heap N[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { a[i] #= heap N(); i = i + 1; }\n"
        "    }\n"
        "    public static int64 run() {\n"
        "        int32 n = " + std::to_string(kOverCap) + ";\n"
        + tail +
        "    }\n"
        "}\n";
}

}  // namespace

// Past the cap the set silently stops counting, so liveCount under-reports by
// however many allocations followed. The count is the instrument every other
// ownership probe in this project reads (BenchTest's three cycle probes, the
// OwnershipLeakProbe suite), so an under-reporting instrument reads as a CLEAN
// run — the failure mode CLAUDE.md 5 warns about: validate the instrument
// before trusting a null result.
TEST(LiveSetGrowthTests, liveCountStaysAccuratePastTheLoadCap) {
    // n instances + the backing array, counted while they are all still live.
    EXPECT_EQ(kOverCap + 1, runI64(program(
        "        int64 base = Cajeta.liveCount();\n"
        "        N[] a #= heap N[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { a[i] #= heap N(); i = i + 1; }\n"
        "        return Cajeta.liveCount() - base;\n")));
}

// The reclamation half: dropping the owner must return the count to baseline
// exactly. An entry the table never recorded is an entry drop cannot claim, so
// the object is never freed -- a leak that scales with how far past the cap the
// program ran.
// The reclamation half. Asserting a zero balance ALONE would be vacuous:
// liveCount reports the TRACKED population, so before the fix it saturated at
// the cap and returned to baseline while leaking everything above it. So this
// program asserts the PEAK first -- proof that every allocation was tracked --
// and only then that the balance is zero. Together they mean reclaimed; either
// alone does not.
TEST(LiveSetGrowthTests, everythingPastTheCapIsStillReclaimed) {
    // The peak is measured INSIDE the helper, where the array is still live,
    // and the balance outside it, after scope exit dropped it. Displacing the
    // local instead (`a = null`, `a #= heap N[0]`) measures nothing: locals
    // have no pre-overwrite drop, so the old value simply stays live.
    EXPECT_EQ(0, runI64(
        "package test;\n"
        "import cajeta.lang.Cajeta;\n"
        "public class N { public int32 v; }\n"
        "public final class D {\n"
        "    static int64 peakOf(int32 n, int64 base) {\n"
        "        N[] a #= heap N[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { a[i] #= heap N(); i = i + 1; }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "    public static int64 run() {\n"
        "        int32 n = " + std::to_string(kOverCap) + ";\n"
        "        D.peakOf(16, 0);\n"                 // warm: one-shot stdlib allocs
        "        int64 base = Cajeta.liveCount();\n"
        "        int64 peak = D.peakOf(n, base);\n"
        // Guard the instrument before trusting the balance: a saturated count
        // returns to baseline while leaking everything above the cap.
        "        if (peak != (int64) n + 1) { return 0 - peak; }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n"));
}

// Growth must not lose a live entry while rehashing, and CHURN must not grow
// the table without bound -- a freed slot leaves a tombstone, and tombstones
// occupy probe sequence exactly as live entries do. Many small cycles whose
// total allocations far exceed the table while the PEAK live set stays tiny:
// the tombstone path is what this exercises, and a lost entry shows up as a
// non-zero balance because its drop can no longer claim it.
TEST(LiveSetGrowthTests, churnRehashesInPlaceWithoutLosingEntries) {
    EXPECT_EQ(0, runI64(
        "package test;\n"
        "import cajeta.lang.Cajeta;\n"
        "public class N { public int32 v; }\n"
        "public final class D {\n"
        "    static void once(int32 n) {\n"
        "        N[] a #= heap N[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { a[i] #= heap N(); i = i + 1; }\n"
        "    }\n"
        "    public static int64 run() {\n"
        "        D.once(64);\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 c = 0;\n"
        // 20000 cycles x 64 = 1.28M allocations through a table that never
        // holds more than 65 at once.
        "        while (c < 20000) { D.once(64); c = c + 1; }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n"));
}
