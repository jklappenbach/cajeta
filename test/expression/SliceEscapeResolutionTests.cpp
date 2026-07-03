// Unit 4 of the slices plan: escape resolution (slice-spec §4, §9).
//
// TDD. The baseline run of an earlier draft PROVED the stakes here are
// correctness, not just precision: storing a slice into an outliving object
// field today stores the BORROWED WRAPPER pointer, and the declaring scope
// frees that wrapper at exit — instant UAF (SIGSEGV reading the field).
// The §4 resolution at escape sites (copy-small / share-large / move-on-
// last-use, arena always copies) is what makes the plain store sound.
//
// Observables: Cajeta.sharedPopulation() (stake count), allocatedBytes()
// (copy vs zero-copy), content-after-source-drop (the wrapper-lifetime
// proof). Sources are HEAP-backed via `#String heapString(n)` except the
// arena test (concat-backed by design).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Shared scaffold: a Keep class with a String field (the escape site is the
// plain `k.v = w` field assign), and a heap-backed source builder.
std::string makeSource(const std::string& runBody) {
    return "package test;\n"
           "public class Keep {\n"
           "    public String v;\n"
           "    public Keep(String v) {\n"
           "        this.v = v;\n"
           "    }\n"
           "}\n"
           "public final class Ut {\n"
           "    public static #String makeBig(int32 doublings) {\n"
           "        String s = \"abcdefgh\";\n"
           "        int32 i = 0;\n"
           "        while (i < doublings) { s = s + s; i = i + 1; }\n"
           "        return s;\n"
           "    }\n"
           "    public static #String heapString(int32 n) {\n"
           "        int8[] buf = Cajeta.allocBytes((int64) n);\n"
           "        int32 i = 0;\n"
           "        while (i < n) {\n"
           "            buf[i] = (int8) (97 + (i - (i / 26) * 26));\n"  // a..z repeating
           "            i = i + 1;\n"
           "        }\n"
           "        return heap String(#buf, n);\n"
           "    }\n"
           "    public static int32 run() {\n"
           "        " + runBody + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& runBody) {
    auto jit = CajetaJit::compile(makeSource(runBody), "test.Ut");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// 4.1.1 (RED until the local-borrow downgrade) — a substring used only
// within its scope takes NO stake (population untouched) and no byte copy.
// Source is a doubling-concat (a large OWNED mode-0 heap buffer — the shape
// that exercises the promote path; heapString's `String(#buf,n)` ctor makes
// mode-1 views, which take no rc and would false-green this).
// DISABLED: awaits 4.2.2's local-borrow downgrade — substring still
// always-promotes at creation (sound interim; re-enable with (c)).
TEST(SliceEscapeResolutionTests, DISABLED_localSubstringIsBorrow) {
    EXPECT_EQ(runJit(
        "String s = makeBig(7);\n"                            // 1 KB owned (drop entry)
        "int64 pop = Cajeta.sharedPopulation();\n"
        "int64 before = Cajeta.allocatedBytes();\n"
        "String w = s.substring(10, 26);\n"                    // local-only view
        "if (w.size() != 16) { return -1; }\n"
        "if (w.charAt(0) != s.charAt(10)) { return -2; }\n"
        "if (Cajeta.sharedPopulation() != pop) { return -3; }\n"   // zero rc
        "if (Cajeta.allocatedBytes() - before > 96) { return -4; }\n" // wrapper only
        "return 1;"), 1);
}

// 4.1.2 (RED) — a ≤threshold slice stored past its scope COPIES: the stored
// value is independent (correct after the source drops), holds no stake on
// the source root, and survives the source scope (no wrapper UAF).
TEST(SliceEscapeResolutionTests, smallEscapeCopies) {
    EXPECT_EQ(runJit(
        "Keep k = heap Keep(\"\");\n"
        "int64 pop = Cajeta.sharedPopulation();\n"
        "{\n"
        "    String s = heapString(64);\n"
        "    String w = s.substring(10, 26);\n"                // 16 B window
        "    k.v = w;\n"                                        // escape: field store
        "}\n"                                                   // s + w drop
        "if (k.v.size() != 16) { return -1; }\n"
        "if (k.v.charAt(0) != (int8) 107) { return -2; }\n"     // 'k' (index 10)
        "if (Cajeta.sharedPopulation() != pop) { return -3; }\n"    // copy: no stake
        "return 1;"), 1);
}

// 4.1.3 (target semantics of the share row) — a >threshold slice stored past
// its scope SHARES: no byte copy at the store, a stake holds the root alive
// through the stored view, content correct after the source scope.
TEST(SliceEscapeResolutionTests, largeEscapeShares) {
    EXPECT_EQ(runJit(
        "Keep k = heap Keep(\"\");\n"
        "int64 pop = Cajeta.sharedPopulation();\n"
        "{\n"
        "    String s = makeBig(8);\n"                          // 2 KB owned (drop entry)
        "    String w = s.substring(100, 612);\n"                // 512 B window
        "    int64 before = Cajeta.allocatedBytes();\n"
        "    k.v = w;\n"                                          // escape: share, no copy
        "    if (Cajeta.allocatedBytes() - before > 128) { return -1; }\n"
        "}\n"                                                     // s + w drop; root pinned
        "if (k.v.size() != 512) { return -2; }\n"
        "if (k.v.charAt(0) != (int8) (97 + (100 - (100 / 8) * 8))) { return -3; }\n"
        "if (Cajeta.sharedPopulation() < pop + 1) { return -4; }\n" // stake held
        "return 1;"), 1);
}

// 4.1.4 — an escaping slice of a frame-arena concat result copies at ANY
// size (the §4 arena row, enforced in the runtime since 6b): valid content
// after the source scope, zero remaining stake.
TEST(SliceEscapeResolutionTests, arenaEscapeCopies) {
    EXPECT_EQ(runJit(
        "Keep k = heap Keep(\"\");\n"
        "int64 pop = Cajeta.sharedPopulation();\n"
        "{\n"
        "    String a = \"abcdefghijklm\";\n"
        "    String b = \"nopqrstuvwxyz\";\n"
        "    String s = a + b;\n"                                // arena-eligible
        "    String w = s.substring(10, 16);\n"                  // materializes if arena
        "    k.v = w;\n"
        "}\n"
        "if (k.v.size() != 6) { return -1; }\n"
        "if (k.v.charAt(0) != (int8) 107) { return -2; }\n"      // 'k'
        "if (Cajeta.sharedPopulation() != pop) { return -3; }\n"
        "return 1;"), 1);
}

// 4.1.5 (GREEN: unchanged discipline) — escaping a borrowed identity object
// through a `#` return stays a compile error.
TEST(SliceEscapeResolutionTests, mutableEscapeStillErrors) {
    std::string src =
        "package test;\n"
        "public class Box {\n"
        "    public int64 n;\n"
        "    public Box(int64 n) {\n"
        "        this.n = n;\n"
        "    }\n"
        "}\n"
        "public final class Ut {\n"
        "    public static #Box leak(Box borrowed) {\n"
        "        return borrowed;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW({
        auto jit = CajetaJit::compile(src, "test.Ut");
        (void) jit;
    });
}

// 4.1.6 (RED) — a large escape whose source view is provably dead after the
// store resolves to a MOVE: no byte copy, no extra rc traffic beyond the
// single stake the stored view holds; exact retirement when detached.
TEST(SliceEscapeResolutionTests, autoMoveOnLastUse) {
    EXPECT_EQ(runJit(
        "Keep k = heap Keep(\"\");\n"
        "int64 pop = Cajeta.sharedPopulation();\n"
        "{\n"
        "    String s = makeBig(8);\n"                          // 2 KB owned (drop entry)
        "    int64 before = Cajeta.allocatedBytes();\n"
        "    String w = s.substring(100, 612);\n"                // 512 B window
        "    k.v = w;\n"                                         // w's LAST use: move
        "    if (Cajeta.allocatedBytes() - before > 192) { return -1; }\n"
        "}\n"
        "if (k.v.size() != 512) { return -2; }\n"
        "if (Cajeta.sharedPopulation() != pop + 1) { return -3; }\n" // exactly one entry
        "k.v = \"\";\n"                                          // detach the stored stake
        "if (Cajeta.sharedPopulation() != pop) { return -4; }\n"     // exact retirement
        "return 1;"), 1);
}

// 4.1.7 (target: unsure ⇒ resolve) — a slice flowing into an interface-typed
// sink (unanalyzable) resolves — never borrows: the stored value stays valid
// past the source scope.
TEST(SliceEscapeResolutionTests, unsureResolves) {
    std::string src =
        "package test;\n"
        "public interface Sink {\n"
        "    public void accept(String v);\n"
        "}\n"
        "public class Holder implements Sink {\n"
        "    public String kept;\n"
        "    public Holder() {\n"
        "        this.kept = \"\";\n"
        "    }\n"
        "    public void accept(String v) {\n"
        "        this.kept = v;\n"                              // callee-side escape site
        "    }\n"
        "}\n"
        "public final class Ut {\n"
        "    public static #String heapString(int32 n) {\n"
        "        int8[] buf = Cajeta.allocBytes((int64) n);\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            buf[i] = (int8) (97 + (i - (i / 26) * 26));\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return heap String(#buf, n);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Holder h = heap Holder();\n"
        "        Sink sink = h;\n"
        "        {\n"
        "            String s = heapString(64);\n"
        "            String w = s.substring(10, 26);\n"
        "            sink.accept(w);\n"
        "        }\n"
        "        if (h.kept.size() != 16) { return -1; }\n"
        "        if (h.kept.charAt(0) != (int8) 107) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Ut");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 1);
}
