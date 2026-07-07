//
// Statement-temp reclamation for call arguments — element-ownership 3.4.3.
//
// A fresh owned String temporary consumed as a method-call argument
// (`d.ignore(x + "!")`, `d.ignore(x.substring(0, 3))`) historically had
// NO owner: the arena pre-pass only routes name-bound concats, the
// callee's borrow formal never takes ownership, and no drop entry was
// registered — one wrapper leaked per call (the pre-Unit-3 String leak
// masked this; the drop fixes made it visible as a liveCount delta).
//
// The fix emits a guarded __cajeta_string_drop for such temps right
// after the call, ONLY when the formal is a borrow. `#T` formals keep
// callee ownership (dropping there would double-free); named locals,
// literals, and dotted reads are untouched.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Live-object delta across an inner scope containing `body` (the
// StackDropClassRefTests harness shape).
int64_t liveDelta(const std::string& classes, const std::string& body) {
    std::string src =
        std::string("package test;\n") + classes +
        "public final class D {\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        if (true) {\n"
        "            " + body + "\n"
        "        }\n"
        "        return Cajeta.liveCount() - base;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int64_t (*)()>("run")();
}

const char* kRoot =
    "public class Root {\n"
    "    public String v;\n"
    "    public Root() { return; }\n"
    "    public void setV(String s) {\n"
    "        this.v = s;\n"
    "    }\n"
    "    public void ignore(String s) { return; }\n"
    "    public void take(#String s) {\n"
    "        this.v = s;\n"
    "    }\n"
    "    public static void sIgnore(String s) { return; }\n"
    "}\n";

} // namespace

// A concat temp passed to a borrow formal is dropped after the call —
// the minimal repro of the leak (one wrapper per call before the fix).
TEST(CallArgTempDropTests, concatTempToBorrowFormalDropped) {
    EXPECT_EQ(liveDelta(kRoot,
        "Root d = stack Root();\n"
        "String x = \"h\";\n"
        "d.ignore(x + \"!\");"), 0);
}

// The original StackDropClassRefTests.diamondSharedBaseFieldDroppedOnce
// shape minus the diamond (which was a red herring): the setter stores
// a borrow, 3A materializes the field copy, and the caller-side temp
// must still be reclaimed.
TEST(CallArgTempDropTests, concatTempThroughBorrowSetterBalances) {
    EXPECT_EQ(liveDelta(kRoot,
        "Root d = stack Root();\n"
        "String x = \"h\";\n"
        "d.setV(x + \"!\");"), 0);
}

// `#T` formal: the callee takes ownership of the temp (stores it; the
// field drop reclaims it when d drops). The caller must NOT also drop —
// this pins the double-free guard.
TEST(CallArgTempDropTests, concatTempToTransferFormalCalleeOwned) {
    EXPECT_EQ(liveDelta(kRoot,
        "Root d = stack Root();\n"
        "String x = \"h\";\n"
        "d.take(x + \"!\");"), 0);
}

// An owned-return method result (substring returns a fresh #String)
// consumed as a borrow argument is the same leak shape as concat.
TEST(CallArgTempDropTests, ownedReturnTempToBorrowFormalDropped) {
    EXPECT_EQ(liveDelta(kRoot,
        "Root d = stack Root();\n"
        "String x = \"hello world\";\n"
        "d.ignore(x.substring(0, 3));"), 0);
}

// A named local passed as a borrow argument keeps its single owner (the
// local's drop entry); the call site must not touch it — the local is
// read again after the call to prove it survived.
TEST(CallArgTempDropTests, namedLocalArgUntouched) {
    EXPECT_EQ(liveDelta(kRoot,
        "Root d = stack Root();\n"
        "String x = \"h\";\n"
        "String y = x + \"!\";\n"
        "d.ignore(y);\n"
        "if (y.count() != 2L) { return 99L; }"), 0);
}

// Static user method with a borrow formal: same reclamation as the
// instance path.
TEST(CallArgTempDropTests, concatTempToStaticBorrowFormalDropped) {
    EXPECT_EQ(liveDelta(kRoot,
        "String x = \"h\";\n"
        "Root.sIgnore(x + \"!\");"), 0);
}

// Nested calls: the inner call's concat temp drops after the inner
// call; the inner result (fresh #String from substring) drops after
// the outer call. Both temps reclaimed, nothing double-freed.
TEST(CallArgTempDropTests, nestedFreshTempsBothReclaimed) {
    EXPECT_EQ(liveDelta(kRoot,
        "Root d = stack Root();\n"
        "String x = \"hello\";\n"
        "d.ignore((x + \" world\").substring(0, 4));"), 0);
}

// The intrinsic print path bypasses formal resolution entirely, so it
// needs its own reclamation: `println("x=" + v)` is the single most
// common concat consumer in real programs (one wrapper leaked per call
// before the fix).
TEST(CallArgTempDropTests, printlnConcatTempDropped) {
    EXPECT_EQ(liveDelta(kRoot,
        "String x = \"h\";\n"
        "System.stdout.println(x + \"!\");"), 0);
}

// A concat temp in a loop: one temp per iteration, all reclaimed —
// the delta stays 0 rather than growing with the trip count.
TEST(CallArgTempDropTests, loopedConcatTempsBounded) {
    EXPECT_EQ(liveDelta(kRoot,
        "Root d = stack Root();\n"
        "String x = \"h\";\n"
        "int32 i = 0;\n"
        "while (i < 100) {\n"
        "    d.ignore(x + \"!\");\n"
        "    i = i + 1;\n"
        "}"), 0);
}
