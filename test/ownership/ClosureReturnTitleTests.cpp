// A closure returned from a method carries its title in the return flag, as a
// class value does: an interior read is a borrow, a fresh closure is owned, and
// a forwarded call rides its callee's flag. Each verdict program returns 0 on pass.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* PRE =
    "package test;\n"
    "import cajeta.lang.Cajeta;\n"
    "public final class A {\n"
    "    static class Cell {\n"
    "        public int32 n;\n"
    "        public Cell(int32 n) { this.n = n; }\n"
    "    }\n"
    "    static int32 takeFn((int32) -> int32 f) { return f(1); }\n"
    "    static class K {\n"
    "        (int32) -> int32 f;\n"
    "        public K(#(int32) -> int32 f) { this.f #= f; }\n"
    "        public (int32) -> int32 get() { return this.f; }\n"
    "        public (int32) -> int32 viaGet() { return this.get(); }\n"
    "    }\n"
    "    static class Link {\n"
    "        (int32) -> int32 next;\n"
    "        (int32) -> int32 entry;\n"
    "        Link() { }\n"
    "    }\n"
    "    static class Chain {\n"
    "        Link[] links;\n"
    "        (int32) -> int32 last;\n"
    "        public Chain() { this.links = null; }\n"
    "        public void build(#(int32) -> int32 terminal) {\n"
    "            this.last #= terminal;\n"
    "            this.links = heap Link[1];\n"
    "            (int32) -> int32 next = this.last;\n"
    "            this.links[0] = heap Link();\n"
    "            Link lk = this.links[0];\n"
    "            lk.next #= next;\n"
    "            (int32) -> int32 entry = (int32 x) -> {\n"
    "                (int32) -> int32 n = lk.next;\n"
    "                return n(x) * 10;\n"
    "            };\n"
    "            lk.entry #= entry;\n"
    "        }\n"
    "        public (int32) -> int32 buildAndReturn(#(int32) -> int32 terminal) {\n"
    "            this.build(#terminal);\n"
    "            return this.links[0].entry;\n"
    "        }\n"
    "    }\n"
    "    static (int32) -> int32 mk(int32 n) {\n"
    "        return (int32 x) -> x + n;\n"
    "    }\n"
    "    static (int32) -> int32 mkLocal(int32 n) {\n"
    "        (int32) -> int32 f = (int32 x) -> x + n;\n"
    "        return f;\n"
    "    }\n"
    "    static (int32) -> int32 viaMk(int32 n) { return A.mk(n); }\n"
    "    static #(int32) -> int32 mkOwned(int32 n) {\n"
    "        return (int32 x) -> x + n;\n"
    "    }\n";

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

std::string loop(const std::string& probeBody, const std::string& want, int failBase) {
    return std::string(PRE)
        + "    static int32 probe() {\n" + probeBody + "    }\n"
        + "    public static int32 run() {\n"
        + "        A.probe(); A.probe();\n"
        + "        int64 l0 = Cajeta.liveCount();\n"
        + "        int32 i = 0;\n"
        + "        while (i < 8) {\n"
        + "            if (A.probe() != " + want + ") { return " + std::to_string(failBase) + "; }\n"
        + "            i = i + 1;\n"
        + "        }\n"
        + "        if (Cajeta.liveCount() != l0) { return " + std::to_string(failBase + 1) + "; }\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
}

const char* KEEPER =
    "        Cell c = heap Cell(3);\n"
    "        K k = heap K((x) -> x + c.n);\n";

}  // namespace

TEST(ClosureReturnTitleTests, instrumentSeesAClosureRecord) {
    std::string src = std::string(PRE)
        + "    static #(int32) -> int32 mkScalar(int32 n) { return (int32 x) -> x + n; }\n"
        + "    public static int32 run() {\n"
        + "        int64 l0 = Cajeta.liveCount();\n"
        + "        (int32) -> int32 g #= A.mkScalar(3);\n"
        + "        int64 l1 = Cajeta.liveCount();\n"
        + "        return (int32) (l1 - l0);\n"
        + "    }\n"
        + "}\n";
    EXPECT_GT(runVerdict(src), 0) << "liveCount must see a closure record, or a balanced count proves nothing";
}

TEST(ClosureReturnTitleTests, fieldGetterBoundPlainIsABorrow) {
    std::string src = loop(std::string(KEEPER)
        + "        (int32) -> int32 g = k.get();\n"
        + "        return g(1);\n", "4", 10);
    EXPECT_EQ(runVerdict(src), 0) << "10 = wrong value; 11 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, fieldGetterBoundSharpIsABorrow) {
    std::string src = loop(std::string(KEEPER)
        + "        (int32) -> int32 g #= k.get();\n"
        + "        return g(1);\n", "4", 20);
    EXPECT_EQ(runVerdict(src), 0) << "20 = wrong value; 21 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, forwardedFieldGetterIsABorrow) {
    std::string src = loop(std::string(KEEPER)
        + "        (int32) -> int32 g = k.viaGet();\n"
        + "        return g(1);\n", "4", 30);
    EXPECT_EQ(runVerdict(src), 0) << "30 = wrong value; 31 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, elementFieldAfterATransferringCallIsABorrow) {
    std::string src = loop(
        "        Cell c = heap Cell(3);\n"
        "        (int32) -> int32 f = (x) -> x + c.n;\n"
        "        Chain ch = heap Chain();\n"
        "        (int32) -> int32 g = ch.buildAndReturn(#f);\n"
        "        return g(1);\n", "40", 40);
    EXPECT_EQ(runVerdict(src), 0) << "40 = wrong value; 41 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, discardedGetterResultIsNotDropped) {
    std::string src = loop(std::string(KEEPER)
        + "        k.get();\n"
        + "        return k.f(1);\n", "4", 50);
    EXPECT_EQ(runVerdict(src), 0) << "50 = wrong value; 51 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, getterResultAsAnArgumentIsLent) {
    std::string src = loop(std::string(KEEPER)
        + "        return A.takeFn(k.get()) + k.f(0);\n", "7", 60);
    EXPECT_EQ(runVerdict(src), 0) << "60 = wrong value; 61 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, freshClosureFromAPlainReturnIsOwned) {
    std::string src = loop(
        "        (int32) -> int32 g = A.mk(3);\n"
        "        return g(1);\n", "4", 70);
    EXPECT_EQ(runVerdict(src), 0) << "70 = wrong value; 71 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, freshClosureLocalFromAPlainReturnIsOwned) {
    std::string src = loop(
        "        (int32) -> int32 g = A.mkLocal(3);\n"
        "        return g(1);\n", "4", 80);
    EXPECT_EQ(runVerdict(src), 0) << "80 = wrong value; 81 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, freshClosureBoundSharpIsOwned) {
    std::string src = loop(
        "        (int32) -> int32 g #= A.mk(3);\n"
        "        return g(1);\n", "4", 90);
    EXPECT_EQ(runVerdict(src), 0) << "90 = wrong value; 91 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, forwardedFreshClosureRidesTheTitle) {
    std::string src = loop(
        "        (int32) -> int32 g = A.viaMk(3);\n"
        "        return g(1);\n", "4", 100);
    EXPECT_EQ(runVerdict(src), 0) << "100 = wrong value; 101 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, ownedReturnIsOwned) {
    std::string src = loop(
        "        (int32) -> int32 g #= A.mkOwned(3);\n"
        "        return g(1);\n", "4", 110);
    EXPECT_EQ(runVerdict(src), 0) << "110 = wrong value; 111 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, freshClosureAsAnArgumentIsDropped) {
    std::string src = loop(
        "        return A.takeFn(A.mk(3));\n", "4", 120);
    EXPECT_EQ(runVerdict(src), 0) << "120 = wrong value; 121 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, discardedFreshClosureIsDropped) {
    std::string src = loop(
        "        A.mk(3);\n"
        "        return 4;\n", "4", 130);
    EXPECT_EQ(runVerdict(src), 0) << "130 = wrong value; 131 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, freshClosureKeptWithASharpStoreIsOwnedByTheKeeper) {
    std::string src = loop(
        "        (int32) -> int32 f = A.mk(3);\n"
        "        K k = heap K(#f);\n"
        "        return k.f(1);\n", "4", 140);
    EXPECT_EQ(runVerdict(src), 0) << "140 = wrong value; 141 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, elementReadBoundPlainIsABorrow) {
    std::string src = loop(
        "        Cell c = heap Cell(3);\n"
        "        (int32) -> int32 f = (x) -> x + c.n;\n"
        "        K k = heap K(#f);\n"
        "        K[] ks = heap K[1];\n"
        "        ks[0] = #k;\n"
        "        (int32) -> int32 g = ks[0].f;\n"
        "        return g(1);\n", "4", 150);
    EXPECT_EQ(runVerdict(src), 0) << "150 = wrong value; 151 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, reassignFromAGetterIsABorrow) {
    std::string src = loop(std::string(KEEPER)
        + "        (int32) -> int32 g = A.mk(0);\n"
        + "        g = k.get();\n"
        + "        return g(1);\n", "4", 160);
    EXPECT_EQ(runVerdict(src), 0) << "160 = wrong value; 161 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, reassignFromAFactoryIsOwned) {
    std::string src = loop(
        "        (int32) -> int32 g = A.mk(0);\n"
        "        g = A.mk(3);\n"
        "        return g(1);\n", "4", 170);
    EXPECT_EQ(runVerdict(src), 0) << "170 = wrong value; 171 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, sharpFieldStoreFromAGetterRecordsTheBorrow) {
    std::string src = loop(std::string(KEEPER)
        + "        K j = heap K(A.mkOwned(0));\n"
        + "        j.f #= k.get();\n"
        + "        return j.f(1);\n", "4", 180);
    EXPECT_EQ(runVerdict(src), 0) << "180 = wrong value; 181 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, sharpFieldStoreFromAFactoryTakesTheTitle) {
    std::string src = loop(
        "        K j = heap K(A.mkOwned(0));\n"
        "        j.f #= A.mk(3);\n"
        "        return j.f(1);\n", "4", 190);
    EXPECT_EQ(runVerdict(src), 0) << "190 = wrong value; 191 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, returnedParameterIsABorrow) {
    std::string src = std::string(PRE)
        + "    static (int32) -> int32 id((int32) -> int32 f) { return f; }\n"
        + "    static int32 probe() {\n"
        + "        Cell c = heap Cell(3);\n"
        + "        (int32) -> int32 f = (x) -> x + c.n;\n"
        + "        (int32) -> int32 g = A.id(f);\n"
        + "        return g(1) + f(0);\n"
        + "    }\n"
        + "    public static int32 run() {\n"
        + "        A.probe(); A.probe();\n"
        + "        int64 l0 = Cajeta.liveCount();\n"
        + "        int32 i = 0;\n"
        + "        while (i < 8) {\n"
        + "            if (A.probe() != 7) { return 200; }\n"
        + "            i = i + 1;\n"
        + "        }\n"
        + "        if (Cajeta.liveCount() != l0) { return 201; }\n"
        + "        return 0;\n"
        + "    }\n"
        + "}\n";
    EXPECT_EQ(runVerdict(src), 0) << "200 = wrong value; 201 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, reassignFromALambdaIsOwned) {
    std::string src = loop(
        "        Cell c = heap Cell(3);\n"
        "        (int32) -> int32 g = A.mk(0);\n"
        "        g = (int32 x) -> x + c.n;\n"
        "        return g(1);\n", "4", 210);
    EXPECT_EQ(runVerdict(src), 0) << "210 = wrong value; 211 = leaked or freed twice";
}

TEST(ClosureReturnTitleTests, borrowedLocalReassignedFromAFactory) {
    std::string src = loop(std::string(KEEPER)
        + "        (int32) -> int32 g = k.get();\n"
        + "        g = A.mk(3);\n"
        + "        return g(1) + k.f(0);\n", "7", 220);
    EXPECT_EQ(runVerdict(src), 0) << "220 = wrong value; 221 = leaked or freed twice";
}
