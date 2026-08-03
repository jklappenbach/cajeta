// transfer-demotes-to-borrow Unit 2 (plan 2.1) — reads of a demoted binding.
//
// `#` moves the TITLE, not the binding. After a transfer the source is a
// BORROW of the same live instance, and a borrow is readable. These tests
// pin that: every read below is an ordinary borrow read.
//
// They fail before 2.2 with CAJETA_ERROR_USE_AFTER_MOVE, which is the point —
// that error names a state the model does not have (spec 1.3).
//
// What stays rejected is transferring again; Unit 3 covers that. Nothing here
// asserts a transfer.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kTag =
    "package test;\n"
    "import cajeta.collection.HashMap;\n"
    "public class Tag {\n"
    "    public int32 n;\n"
    "    public Tag(int32 v) { this.n = v; }\n"
    "    public void setValue(int32 v) { this.n = v; }\n"
    "}\n"
    "public class Holder {\n"
    "    public Tag f;\n"
    "    public Tag g;\n"
    "    public Holder() { this.f = null; this.g = null; }\n"
    "}\n";

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}

}  // namespace

// 2.1.1 — the false positive from spec 1.2. `owner` is a local in the same
// scope, so the instance outlives every use of `orig`; the write through the
// demoted binding must land on the same object.
TEST(DemotedBindingReadTests, methodCallOnDemotedLocalMutatesSameInstance) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tag orig = heap Tag(1);\n"
        "        Tag owner #= orig;\n"
        "        orig.setValue(5);\n"
        "        return owner.n;\n"        // proves it is ONE instance
        "    }\n"
        "}\n"), 5);
}

// 2.1.2 — a field read through the demoted binding yields the live value.
TEST(DemotedBindingReadTests, fieldReadOfDemotedLocal) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tag orig = heap Tag(7);\n"
        "        Tag owner #= orig;\n"
        "        return orig.n;\n"
        "    }\n"
        "}\n"), 7);
}

// 2.1.3 — a demoted binding is lendable: passing it to a plain formal is an
// ordinary borrow.
TEST(DemotedBindingReadTests, demotedLocalPassedAsPlainArgument) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 peek(Tag t) { return t.n; }\n"
        "    public static int32 run() {\n"
        "        Tag orig = heap Tag(9);\n"
        "        Tag owner #= orig;\n"
        "        return D.peek(orig);\n"
        "    }\n"
        "}\n"), 9);
}

// 2.1.4 — the case the whole spec exists for. The map owns the key; `t` is a
// borrow of that same instance, so an identity-hashed class key resolves.
TEST(DemotedBindingReadTests, mapLookupAfterSurrenderingTheKey) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>();\n"
        "        Tag t = heap Tag(1);\n"
        "        m.put(#t, 42);\n"
        "        return m.get(t);\n"
        "    }\n"
        "}\n"), 42);
}

// 2.1.5 — a demoted PATH is readable too (spec 2.3.1). The path machinery is
// separate from the identifier machinery, so it needs its own pin.
TEST(DemotedBindingReadTests, demotedPathIsReadable) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        h.f #= heap Tag(3);\n"
        "        Tag taken #= h.f;\n"
        "        return h.f.n;\n"          // h.f demoted, still readable
        "    }\n"
        "}\n"), 3);
}

// 2.1.6 — demoting one path says nothing about a sibling (spec 2.3.2). Guards
// against a prefix match that over-demotes.
TEST(DemotedBindingReadTests, siblingPathIsUnaffectedByDemotion) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        h.f #= heap Tag(3);\n"
        "        h.g #= heap Tag(4);\n"
        "        Tag taken #= h.f;\n"
        "        return h.g.n;\n"          // g was never demoted
        "    }\n"
        "}\n"), 4);
}

// 2.1.7 — a local demoted by CLOSURE CAPTURE is readable afterwards
// (spec 7.2, adopted at approval). Called out separately because a closure's
// lifetime is the least obvious of the demotion sites.
TEST(DemotedBindingReadTests, localDemotedByClosureCaptureIsReadable) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tag orig = heap Tag(11);\n"
        "        () -> int32 f = () -> {\n"
        "            return orig.n;\n"
        "        };\n"
        "        return orig.n;\n"
        "    }\n"
        "}\n"), 11);
}
