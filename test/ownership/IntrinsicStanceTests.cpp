// ownership-title-classifier 8.2.4 — an intrinsic lowering's result takes its
// DECLARED stance, never a return-flag read: the stub never runs, so the flag
// left in the TLS is the previous call's. Each program returns 0 on pass.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"
#include "cajeta/ownership/OwnedBindCheck.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#ifndef _WIN32
#include <unistd.h>
#endif

using cajeta_test::CajetaJit;

namespace {

// A 16-byte fixture under the test tmp root — never the system /tmp by default.
std::string fixturePath(const char* name) {
    std::string root;
    if (const char* r = std::getenv("TEST_TMPDIR"); r && *r) root = r;
    else if (const char* t = std::getenv("TMPDIR"); t && *t) root = t;
    else root = "tmp";
    std::filesystem::create_directories(root);
    std::string path = root + "/cajeta_intrinsic_stance_"
#ifndef _WIN32
        + std::to_string((long long) ::getpid()) + "_"
#endif
        + name + ".bin";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    for (int i = 0; i < 16; i++) out.put((char) (i + 1));
    return path;
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.io.file.File;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 n;\n"
    "        public Cell(int32 n) { this.n = n; }\n"
    "        public Cell self() { return this; }\n"     // a plain `this` return leaves 0 in the TLS
    "    }\n"
    "    static #Cell mk() { return heap Cell(1); }\n";  // a `#` return leaves 1

int32_t runVerdict(const std::string& src) {
    try {
        auto jit = CajetaJit::compile(src.c_str(), "test.A");
        if (!jit) return -1;
        auto fn = jit->lookup<int32_t (*)()>("run");
        if (!fn) return -2;
        return fn();
    } catch (cajeta::Exception& e) {
        ADD_FAILURE() << "unexpected rejection: " << e.getErrorId() << ": " << e.getMessage();
        return -3;
    }
}

// `probeBody` computes an int32 the loop checks against `want`, twice to warm.
std::string loop(const std::string& members, const std::string& path,
                 const std::string& probeBody, const std::string& want, int failBase) {
    return std::string(PRE) + members
        + "    static int32 probe(String p) {\n" + probeBody + "    }\n"
        + "    public static int32 run() {\n"
        + "        String p = \"" + path + "\";\n"
        + "        A.probe(p); A.probe(p);\n"
        + "        int64 l0 = Cajeta.liveCount();\n"
        + "        int32 i = 0;\n"
        + "        while (i < 8) {\n"
        + "            if (A.probe(p) != " + want + ") { return " + std::to_string(failBase) + "; }\n"
        + "            i = i + 1;\n"
        + "        }\n"
        + "        if (Cajeta.liveCount() != l0) { return " + std::to_string(failBase + 1) + "; }\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
}

} // namespace

// The leak: a borrow-returning call right before the intrinsic bind.
TEST(IntrinsicStanceTests, staleBorrowFlagDoesNotLeakAnOwnedIntrinsicBind) {
    std::string src = loop("", fixturePath("bind0"),
        "        Cell c = heap Cell(1);\n"
        "        Cell b = c.self();\n"                       // TLS <- 0
        "        int8[] raw #= File.readAllBytes(p);\n"     // declared `#int8[]`: OWNED, statically
        "        return b.n + (int32) raw.count();\n", "17", 10);
    EXPECT_EQ(runVerdict(src), 0) << "10 = wrong bytes; 11 = the buffer leaked (a stale TLS read said borrow)";
}

// The control: a title-returning call before it — the instrument sees the buffer.
TEST(IntrinsicStanceTests, ownedFlagBeforeAnIntrinsicBindIsBalancedToo) {
    std::string src = loop("", fixturePath("bind1"),
        "        Cell c #= A.mk();\n"                        // TLS <- 1
        "        int8[] raw #= File.readAllBytes(p);\n"
        "        return c.n + (int32) raw.count();\n", "17", 20);
    EXPECT_EQ(runVerdict(src), 0) << "20 = wrong bytes; 21 = the buffer leaked or was freed twice";
}

// The same read at a FIELD store.
TEST(IntrinsicStanceTests, staleBorrowFlagDoesNotLeakAnOwnedIntrinsicFieldStore) {
    std::string src = loop(
        "    static class Keeper {\n"
        "        int8[] raw;\n"
        "        public Keeper() { this.raw = null; }\n"
        "        public void load(String p) {\n"
        "            Cell c = heap Cell(1);\n"
        "            Cell b = c.self();\n"                   // TLS <- 0
        "            this.raw #= File.readAllBytes(p);\n"   // the slot owns the buffer
        "            return;\n"
        "        }\n"
        "    }\n", fixturePath("store0"),
        "        Keeper k = heap Keeper();\n"
        "        k.load(p);\n"
        "        return (int32) k.raw.count();\n", "16", 30);
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong bytes; 31 = the buffer leaked (the store recorded a borrow)";
}

// Returned through a wrapper: the declared title rides out of a `#` and a
// plain return alike, so the caller's `#=` owns the buffer either way.
TEST(IntrinsicStanceTests, intrinsicResultReturnedThroughWrappersKeepsItsTitle) {
    std::string src = loop(
        "    static #int8[] loadOwned(String p) { return File.readAllBytes(p); }\n"
        "    static int8[] loadPlain(String p) { return File.readAllBytes(p); }\n",
        fixturePath("wrap0"),
        "        Cell c = heap Cell(1);\n"
        "        Cell b = c.self();\n"                       // TLS <- 0
        "        int8[] r1 #= A.loadOwned(p);\n"
        "        Cell d = c.self();\n"                       // TLS <- 0 again
        "        int8[] r2 #= A.loadPlain(p);\n"
        "        return b.n + (int32) r1.count() + (int32) r2.count();\n", "33", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = wrong bytes; 41 = a buffer leaked (a wrapper's return flag went stale)";
}

// An intrinsic with NO declaration at all (`Cajeta.allocBytes`, `System.env.get`):
// its result is a fresh allocation, owned, whatever the TLS holds.
TEST(IntrinsicStanceTests, undeclaredIntrinsicArrayResultIsOwned) {
    std::string src = loop("", fixturePath("alloc0"),
        "        Cell c = heap Cell(1);\n"
        "        Cell b = c.self();\n"                       // TLS <- 0
        "        int8[] buf = Cajeta.allocBytes(16L);\n"    // no stub, no flag: a fresh buffer
        "        buf[0] = 3;\n"
        "        return b.n + (int32) buf.count();\n", "17", 50);
    EXPECT_EQ(runVerdict(src), 0) << "50 = wrong count; 51 = the allocBytes buffer leaked";
}

TEST(IntrinsicStanceTests, undeclaredIntrinsicStringResultIsOwned) {
    std::string src = loop("", fixturePath("env0"),
        "        Cell c = heap Cell(1);\n"
        "        Cell b = c.self();\n"                       // TLS <- 0
        "        String home = System.env.get(\"PATH\");\n"  // a fresh wrapper around getenv
        "        if (home == null) { return 0 - 1; }\n"
        "        return b.n + (home.byteLength() > 0 ? 16 : 0);\n", "17", 60);
    EXPECT_EQ(runVerdict(src), 0) << "60 = PATH unset or wrong; 61 = the env String leaked";
}

// The declaration still feeds the owned-bind rule (§4.6): a plain `=` of the
// `#int8[]` intrinsic is rejected, as for any `#R` call.
TEST(IntrinsicStanceTests, plainBindOfAnOwnedIntrinsicStillNeedsTransfer) {
    cajeta::ownership::setOwnedBindWarns(false);
    std::string src = std::string(PRE)
        + "    public static int32 run() {\n"
        + "        int8[] raw = File.readAllBytes(\"" + fixturePath("plain0") + "\");\n"
        + "        return (int32) raw.count();\n"
        + "    }\n"
        + "}\n";
    bool rejected = false;
    try {
        auto jit = CajetaJit::compile(src.c_str(), "test.A");
        (void) jit;
    } catch (cajeta::Exception& e) {
        rejected = true;
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER") << e.getMessage();
    }
    cajeta::ownership::clearOwnedBindWarnsOverride();
    EXPECT_TRUE(rejected) << "a plain `=` of an owned intrinsic result compiled";
}

// An ARRAY's `xs.stream()` is compiler sugar with no resolved method, so as a
// temp receiver it is a constant owned one and the reclaim drops it.
TEST(IntrinsicStanceTests, arrayStreamTempReceiverIsReclaimedAfterAVoidTerminal) {
    std::string src = loop(
        "    static class Acc {\n"
        "        public int32 t;\n"
        "        public Acc() { this.t = 0; }\n"
        "        public void bump(int32 x) { this.t = this.t + x; }\n"
        "    }\n", "unused",
        "        int32[] xs = [1, 2, 3, 4];\n"
        "        Acc acc = heap Acc();\n"
        "        (int32) -> void f = (x) -> acc.bump(x);\n"      // a closure LOCAL: its frame drops it (a closure
        "        xs.stream().forEach(f);\n"                      // TEMP argument is never dropped — plan 8.2.2)
        "        return acc.t;\n", "10", 80);
    EXPECT_EQ(runVerdict(src), 0) << "80 = wrong sum; 81 = the array stream leaked (or was freed twice)";
}

TEST(IntrinsicStanceTests, arrayStreamTempReceiverIsReclaimedAfterAScalarTerminal) {
    std::string src = loop("", "unused",
        "        int32[] xs = [1, 2, 3, 4];\n"
        "        return xs.stream().count();\n", "4", 90);
    EXPECT_EQ(runVerdict(src), 0) << "90 = wrong count; 91 = the array stream leaked (or was freed twice)";
}

// A CLOSURE call has no resolved method either, but it is not an intrinsic: its
// body stores the return flag, so a lambda returning its parameter stays a
// borrow (foldWorker's shape) and the frame frees nothing.
TEST(IntrinsicStanceTests, closureCallResultReturningItsParameterStaysABorrow) {
    std::string src = loop(
        "    static int32 bump(Cell keep, (Cell, int32) -> #Cell fn) {\n"
        "        Cell acc = keep;\n"                       // a borrow
        "        acc = fn(acc, 1);\n"                      // rides the closure's flag: 0
        "        acc = fn(acc, 2);\n"
        "        return acc.n;\n"
        "    }\n", "unused",
        "        Cell keep = heap Cell(1);\n"
        "        int32 v = A.bump(keep, (acc, x) -> { acc.n = acc.n + x; return acc; });\n"
        "        Cell other = heap Cell(100);\n"           // would reuse a freed keep
        "        return keep.n * 10 + v;\n", "44", 100);
    EXPECT_EQ(runVerdict(src), 0) << "100 = keep was freed under the borrow (or a wrong sum); 101 = a cell leaked or was freed twice";
}

// The owned side: a lambda returning a fresh value hands its title out.
TEST(IntrinsicStanceTests, closureCallResultReturningAFreshValueIsOwned) {
    std::string src = loop("", "unused",
        "        () -> #Cell mk = () -> heap Cell(3);\n"
        "        Cell a #= mk();\n"
        "        Cell b #= mk();\n"
        "        return a.n + b.n;\n", "6", 110);
    EXPECT_EQ(runVerdict(src), 0) << "110 = wrong sum; 111 = a fresh cell leaked (constant borrow) or was freed twice";
}
