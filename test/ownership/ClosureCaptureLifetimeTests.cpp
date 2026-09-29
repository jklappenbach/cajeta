// A lambda never takes the title of what it captures: a capture is a borrow.
// What happens to an object a lambda captures, in the factory-into-keeper shape
// a middleware takes: a factory builds a lambda over a config, stores it in a
// keeper, and returns the keeper. Each probe returns
// goneAfterFactory * 10000 + valueReadThroughKeeper * 10 + goneAfterKeeperDrop,
// with an allocation churn between the factory and the read, so a dangling
// capture reads reused memory rather than the config's value.

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
    "import cajeta.lang.String;\n"
    "public final class A {\n"
    "    static class Cfg {\n"
    "        public static int32 gone = 0;\n"
    "        public int32 v;\n"
    "        public Cfg(int32 v) { this.v = v; }\n"
    "        public ~Cfg() { Cfg.gone = Cfg.gone + 1; }\n"
    "    }\n"
    "    static class Junk {\n"
    "        public int32 a;\n"
    "        public int32 b;\n"
    "        public Junk() { this.a = 999; this.b = 999; }\n"
    "    }\n"
    "    static class K {\n"
    "        (int32) -> int32 f;\n"
    "        public K((int32) -> int32 f) { this.f #= f; }\n"
    "        public int32 call(int32 x) { return this.f(x); }\n"
    "    }\n"
    "    static class KOwn {\n"
    "        (int32) -> int32 f;\n"
    "        Cfg owned;\n"
    "        public KOwn() { }\n"
    "        public int32 call(int32 x) { return this.f(x); }\n"
    "    }\n"
    "    static void churn() {\n"
    "        int32 i = 0;\n"
    "        while (i < 64) { Junk j = heap Junk(); i = i + 1; }\n"
    "    }\n";

int32_t run(const std::string& members, const std::string& body) {
    std::string src = std::string(PRE) + members
        + "    public static int32 run() {\n" + body + "    }\n}\n";
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

const char* READ_AND_DROP =
    "        int32 after = 0;\n"
    "        int32 seen = 0;\n"
    "        {\n"
    "            K k #= A.make();\n"
    "            after = Cfg.gone;\n"
    "            A.churn();\n"
    "            seen = k.call(0);\n"
    "        }\n"
    "        return after * 10000 + seen * 10 + Cfg.gone;\n";

} // namespace

// A `#` formal captured by the lambda.
TEST(ClosureCaptureLifetimeTests, capturedSharpFormal) {
    int32_t r = run(
        "    static #K build(#Cfg cfg) {\n"
        "        return heap K((x) -> x + cfg.v);\n"
        "    }\n"
        "    static #K make() { return A.build(heap Cfg(7)); }\n",
        READ_AND_DROP);
    EXPECT_EQ(r / 10000, 1) << "the capture does not keep a # formal alive: it is freed when the factory returns";
    EXPECT_EQ(r % 10, 1) << "and it is freed once, not again with the keeper";
}

// A local the factory made itself, captured by the lambda.
TEST(ClosureCaptureLifetimeTests, capturedLocal) {
    int32_t r = run(
        "    static #K make() {\n"
        "        Cfg cfg = heap Cfg(7);\n"
        "        return heap K((x) -> x + cfg.v);\n"
        "    }\n",
        READ_AND_DROP);
    EXPECT_EQ(r / 10000, 1) << "a captured local is freed when the factory returns";
    EXPECT_EQ(r % 10, 1) << "and it is freed once";
}

// A plain formal the caller keeps owning, captured by the lambda.
TEST(ClosureCaptureLifetimeTests, capturedPlainFormalCallerKeeps) {
    int32_t r = run(
        "    static #K build(Cfg cfg) {\n"
        "        return heap K((x) -> x + cfg.v);\n"
        "    }\n",
        "        Cfg cfg = heap Cfg(7);\n"
        "        int32 seen = 0;\n"
        "        int32 afterKeeper = 0;\n"
        "        {\n"
        "            K k #= A.build(cfg);\n"
        "            A.churn();\n"
        "            seen = k.call(0);\n"
        "        }\n"
        "        afterKeeper = Cfg.gone;\n"
        "        return afterKeeper * 10000 + seen * 10 + Cfg.gone;\n");
    EXPECT_EQ(r, 70) << "a borrow the caller keeps alive reads correctly and the keeper frees nothing";
}

// The keeper owns the config in a field and the lambda borrows it from there.
TEST(ClosureCaptureLifetimeTests, keeperOwnsTheCaptureInAField) {
    int32_t r = run(
        "    static #KOwn build(#Cfg cfg) {\n"
        "        KOwn k = heap KOwn();\n"
        "        (int32) -> int32 f = (x) -> x + cfg.v;\n"
        "        k.f #= f;\n"
        "        k.owned = #cfg;\n"
        "        return #k;\n"
        "    }\n"
        "    static #KOwn make() { return A.build(heap Cfg(7)); }\n",
        "        int32 after = 0;\n"
        "        int32 seen = 0;\n"
        "        {\n"
        "            KOwn k #= A.make();\n"
        "            after = Cfg.gone;\n"
        "            A.churn();\n"
        "            seen = k.call(0);\n"
        "        }\n"
        "        return after * 10000 + seen * 10 + Cfg.gone;\n");
    EXPECT_EQ(r, 71) << "kept alive by the keeper, read correctly, freed once with it";
}

// `#cfg` inside the lambda body, the transfer spelling the lambdas skill names.
TEST(ClosureCaptureLifetimeTests, sharpInsideTheBodyFromASharpFormal) {
    int32_t r = run(
        "    static #K build(#Cfg cfg) {\n"
        "        return heap K((x) -> x + #cfg.v);\n"
        "    }\n"
        "    static #K make() { return A.build(heap Cfg(7)); }\n",
        READ_AND_DROP);
    EXPECT_EQ(r, 71) << "#cfg in member position moves the formal into the closure";
}

TEST(ClosureCaptureLifetimeTests, sharpInsideTheBodyFromALocal) {
    int32_t r = run(
        "    static #K make() {\n"
        "        Cfg cfg = heap Cfg(7);\n"
        "        return heap K((x) -> x + #cfg.v);\n"
        "    }\n",
        READ_AND_DROP);
    EXPECT_EQ(r, 71) << "#cfg in member position moves the local into the closure";
}

// `#cfg` as a call argument inside the body moves the capture into the closure,
// as the member-position spelling does (was 110001: freed at the factory).
TEST(ClosureCaptureLifetimeTests, sharpArgumentInsideTheBodyMovesIntoTheClosure) {
    int32_t r = run(
        "    static int32 use(Cfg c, int32 x) { return x + c.v; }\n"
        "    static #K build(#Cfg cfg) {\n"
        "        return heap K((x) -> A.use(#cfg, x));\n"
        "    }\n"
        "    static #K make() { return A.build(heap Cfg(7)); }\n",
        "        int32 after = 0;\n"
        "        int32 first = 0;\n"
        "        int32 second = 0;\n"
        "        int32 between = 0;\n"
        "        {\n"
        "            K k #= A.make();\n"
        "            after = Cfg.gone;\n"
        "            first = k.call(0);\n"
        "            between = Cfg.gone;\n"
        "            A.churn();\n"
        "            second = k.call(0);\n"
        "        }\n"
        "        return after * 100000 + between * 10000 + first * 1000 + second * 10 + Cfg.gone;\n");
    EXPECT_EQ(r, 7071) << "alive after the factory, read correctly twice, freed once with the keeper";
}

// `#cfg` as a constructor argument inside the body moves the capture into the
// closure too. A capture seen only inside `heap X(...)` must be collected at all.
TEST(ClosureCaptureLifetimeTests, sharpCreatorArgumentInsideTheBodyMovesIntoTheClosure) {
    int32_t r = run(
        "    static class Box {\n"
        "        Cfg c;\n"
        "        public Box(Cfg c) { this.c #= c; }\n"
        "        public int32 read(int32 x) { return x + this.c.v; }\n"
        "    }\n"
        "    static int32 open(Box b, int32 x) { return b.read(x); }\n"
        "    static #K build(#Cfg cfg) {\n"
        "        return heap K((x) -> A.open(heap Box(#cfg), x));\n"
        "    }\n"
        "    static #K make() { return A.build(heap Cfg(7)); }\n",
        "        int32 after = 0;\n"
        "        int32 first = 0;\n"
        "        int32 second = 0;\n"
        "        int32 between = 0;\n"
        "        {\n"
        "            K k #= A.make();\n"
        "            after = Cfg.gone;\n"
        "            first = k.call(0);\n"
        "            between = Cfg.gone;\n"
        "            A.churn();\n"
        "            second = k.call(0);\n"
        "        }\n"
        "        return after * 100000 + between * 10000 + first * 1000 + second * 10 + Cfg.gone;\n");
    EXPECT_EQ(r, 7071) << "alive after the factory, read correctly twice, freed once with the keeper";
}
