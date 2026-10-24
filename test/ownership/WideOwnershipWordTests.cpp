// The hidden per-instance ownership word is one i64: one bit per class-reference
// or array field. A class with more than 64 such fields indexed past it with no
// diagnostic, and an optimized build then corrupted itself two steps after the
// store (cajeta-llm CausalLM at 66 fields, 2026-10-24: a release-only SIGSEGV in
// an untouched method). These pin the 65th field and beyond: a title stored past
// bit 63 must be its own bit, never field f0's.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <string>
using cajeta_test::CajetaJit;

namespace {

// A class with 70 bit-carrying fields; `body` is run()'s body and returns an int32.
std::string wideSource(const std::string& body) {
    std::string s =
        "package test;\n"
        "import cajeta.lang.Cajeta;\n"
        "public final class A {\n"
        "    static class Cell {\n"
        "        public int32 n;\n"
        "        public Cell(int32 n) { this.n = n; }\n"
        "    }\n"
        "    static class Wide {\n"
        "        public Cell f0;\n"
        "        public Cell f1;\n"
        "        public Cell f2;\n"
        "        public Cell f3;\n"
        "        public Cell f4;\n"
        "        public Cell f5;\n"
        "        public Cell f6;\n"
        "        public Cell f7;\n"
        "        public Cell f8;\n"
        "        public Cell f9;\n"
        "        public Cell f10;\n"
        "        public Cell f11;\n"
        "        public Cell f12;\n"
        "        public Cell f13;\n"
        "        public Cell f14;\n"
        "        public Cell f15;\n"
        "        public Cell f16;\n"
        "        public Cell f17;\n"
        "        public Cell f18;\n"
        "        public Cell f19;\n"
        "        public Cell f20;\n"
        "        public Cell f21;\n"
        "        public Cell f22;\n"
        "        public Cell f23;\n"
        "        public Cell f24;\n"
        "        public Cell f25;\n"
        "        public Cell f26;\n"
        "        public Cell f27;\n"
        "        public Cell f28;\n"
        "        public Cell f29;\n"
        "        public Cell f30;\n"
        "        public Cell f31;\n"
        "        public Cell f32;\n"
        "        public Cell f33;\n"
        "        public Cell f34;\n"
        "        public Cell f35;\n"
        "        public Cell f36;\n"
        "        public Cell f37;\n"
        "        public Cell f38;\n"
        "        public Cell f39;\n"
        "        public Cell f40;\n"
        "        public Cell f41;\n"
        "        public Cell f42;\n"
        "        public Cell f43;\n"
        "        public Cell f44;\n"
        "        public Cell f45;\n"
        "        public Cell f46;\n"
        "        public Cell f47;\n"
        "        public Cell f48;\n"
        "        public Cell f49;\n"
        "        public Cell f50;\n"
        "        public Cell f51;\n"
        "        public Cell f52;\n"
        "        public Cell f53;\n"
        "        public Cell f54;\n"
        "        public Cell f55;\n"
        "        public Cell f56;\n"
        "        public Cell f57;\n"
        "        public Cell f58;\n"
        "        public Cell f59;\n"
        "        public Cell f60;\n"
        "        public Cell f61;\n"
        "        public Cell f62;\n"
        "        public Cell f63;\n"
        "        public Cell f64;\n"
        "        public Cell f65;\n"
        "        public Cell f66;\n"
        "        public Cell f67;\n"
        "        public Cell f68;\n"
        "        public Cell f69;\n"
        "        public Wide() { return; }\n"
        "    }\n"
        "    public static int32 run() {\n";
    s += body;
    s += "    }\n}\n";
    return s;
}

int32_t runWide(const std::string& body) {
    auto jit = CajetaJit::compile(wideSource(body).c_str(), "test.A");
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

} // namespace

// A borrow in f0 and a title in f64: f64's title is its own bit, so the holder's
// drop frees f64's cell and leaves the borrowed cell to its owner. With f64 aliased
// onto bit 0, the drop freed the borrow and the owner freed it again.
TEST(WideOwnershipWordTests, aTitlePastBit63IsNotFieldZerosBit) {
    const int32_t r = runWide(
        "        int64 live0 = Cajeta.liveCount();\n"
        "        {\n"
        "            Cell shared #= heap Cell(1);\n"
        "            Wide w #= heap Wide();\n"
        "            w.f0 = shared;\n"
        "            w.f64 #= heap Cell(64);\n"
        "            w.f69 #= heap Cell(69);\n"
        "            if (w.f64.n != 64 || w.f69.n != 69 || w.f0.n != 1) { return 100; }\n"
        "        }\n"
        "        return (int32) (Cajeta.liveCount() - live0);\n");
    EXPECT_EQ(r, 0) << "live-count delta after the holder and its owner dropped";
}

// Every field past bit 63 holds its own title: 70 owned cells go in, 70 come out.
TEST(WideOwnershipWordTests, seventyOwnedFieldsAllFreeOnDrop) {
    std::string body = "        int64 live0 = Cajeta.liveCount();\n        {\n            Wide w #= heap Wide();\n";
    for (int i = 0; i < 70; ++i) body += "            w.f" + std::to_string(i) + " #= heap Cell(" + std::to_string(i) + ");\n";
    body += "            if (w.f0.n != 0 || w.f63.n != 63 || w.f64.n != 64 || w.f69.n != 69) { return 100; }\n";
    body += "        }\n        return (int32) (Cajeta.liveCount() - live0);\n";
    EXPECT_EQ(runWide(body), 0) << "live-count delta: a bit that aliased would leak or double-free";
}

// The control at the edge: 64 fields exactly behaved before and must still.
TEST(WideOwnershipWordTests, sixtyFourFieldsStillFreeOnDrop) {
    std::string body = "        int64 live0 = Cajeta.liveCount();\n        {\n            Wide w #= heap Wide();\n";
    for (int i = 0; i < 64; ++i) body += "            w.f" + std::to_string(i) + " #= heap Cell(" + std::to_string(i) + ");\n";
    body += "        }\n        return (int32) (Cajeta.liveCount() - live0);\n";
    EXPECT_EQ(runWide(body), 0);
}
