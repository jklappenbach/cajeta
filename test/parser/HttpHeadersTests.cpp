// NET-7.2 — cajeta.io.net.Headers tests.
//
// `Headers` is a pure-logic, case-insensitive, multi-value,
// insertion-order-preserving HTTP header map (no I/O), so it tests
// directly over the JIT: each test compiles a small Cajeta `run()`
// that drives the map and returns an int32 sentinel — positive on
// success, a distinct negative per failed sub-check (the same
// convention JsonReaderTests / StringMethodsTests use).
//
// Pins the NET-7.2 acceptance criterion:
//   "Header map is case-insensitive on get + preserves multi-value
//    order." → HttpHeadersTests.caseInsensitiveMultiValue.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.io.net.Headers;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Case-insensitive get: a header added with mixed case is retrievable
// under any case (the wire is case-insensitive; keys are lowercased on
// insert).
TEST(HttpHeadersTests, caseInsensitiveGet) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"Content-Type\", \"text/html\");\n"
        "String v1 #= h.get(\"content-type\");\n"
        "String v2 #= h.get(\"CONTENT-TYPE\");\n"
        "String v3 #= h.get(\"Content-Type\");\n"
        "if (v1 == null) return -1;\n"
        "if (!v1.equals(\"text/html\")) return -2;\n"
        "if (!v2.equals(\"text/html\")) return -3;\n"
        "if (!v3.equals(\"text/html\")) return -4;\n"
        "return 1;"), 1);
}

// Absent header → get returns null, getAll returns a length-0 array,
// contains is false.
TEST(HttpHeadersTests, absentHeaderReturnsNull) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"Host\", \"example.test\");\n"
        "if (h.get(\"x-missing\") != null) return -1;\n"
        "if (h.contains(\"x-missing\")) return -2;\n"
        "String[] all #= h.getAll(\"x-missing\");\n"
        "if (all.count() != 0) return -3;\n"
        "return 1;"), 1);
}

// The headline acceptance test: case-insensitive get AND multi-value
// order preservation in one. Two repeated headers (added under
// differing case) come back via getAll in insertion order; get folds
// them with ", ".
TEST(HttpHeadersTests, caseInsensitiveMultiValue) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"Accept\", \"text/html\");\n"
        "h.add(\"ACCEPT\", \"application/json\");\n"
        "h.add(\"accept\", \"text/plain\");\n"
        // getAll preserves insertion order regardless of header case.
        "String[] all #= h.getAll(\"Accept\");\n"
        "if (all.count() != 3) return -1;\n"
        "if (!all[0].equals(\"text/html\")) return -2;\n"
        "if (!all[1].equals(\"application/json\")) return -3;\n"
        "if (!all[2].equals(\"text/plain\")) return -4;\n"
        // get() is case-insensitive and comma-folds in order.
        "String folded #= h.get(\"aCCePt\");\n"
        "if (folded == null) return -5;\n"
        "if (!folded.equals(\"text/html, application/json, text/plain\")) return -6;\n"
        "return 1;"), 1);
}

// add appends (multi-value); set is last-write-wins (collapses to one).
TEST(HttpHeadersTests, setReplacesAllOccurrences) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"X-Tag\", \"a\");\n"
        "h.add(\"X-Tag\", \"b\");\n"
        "if (h.getAll(\"x-tag\").count() != 2) return -1;\n"
        "h.set(\"x-tag\", \"only\");\n"
        "String[] all #= h.getAll(\"X-Tag\");\n"
        "if (all.count() != 1) return -2;\n"
        "if (!all[0].equals(\"only\")) return -3;\n"
        "if (!h.get(\"X-TAG\").equals(\"only\")) return -4;\n"
        "return 1;"), 1);
}

// remove drops every slot for a name and compacts, preserving the
// insertion order of the survivors.
TEST(HttpHeadersTests, removeCompactsPreservingOrder) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"A\", \"1\");\n"
        "h.add(\"B\", \"2\");\n"
        "h.add(\"A\", \"3\");\n"
        "h.add(\"C\", \"4\");\n"
        "h.remove(\"a\");\n"
        "if (h.contains(\"A\")) return -1;\n"
        "if (h.count() != 2) return -2;\n"
        // Survivors B, C keep their order at slots 0,1.
        "if (!h.nameAt(0).equals(\"b\")) return -3;\n"
        "if (!h.valueAt(0).equals(\"2\")) return -4;\n"
        "if (!h.nameAt(1).equals(\"c\")) return -5;\n"
        "if (!h.valueAt(1).equals(\"4\")) return -6;\n"
        "return 1;"), 1);
}

// Set-Cookie is the comma-fold exception (RFC 6265): get() returns the
// FIRST value only (folding would be lossy — cookie Expires dates
// contain commas), while getAll() still exposes every value in order.
TEST(HttpHeadersTests, setCookieIsNotFolded) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"Set-Cookie\", \"a=1\");\n"
        "h.add(\"set-cookie\", \"b=2\");\n"
        // get() must NOT fold Set-Cookie: returns the first value.
        "String first #= h.get(\"Set-Cookie\");\n"
        "if (first == null) return -1;\n"
        "if (!first.equals(\"a=1\")) return -2;\n"
        // getAll() exposes both, in order.
        "String[] all #= h.getAll(\"set-cookie\");\n"
        "if (all.count() != 2) return -3;\n"
        "if (!all[0].equals(\"a=1\")) return -4;\n"
        "if (!all[1].equals(\"b=2\")) return -5;\n"
        "return 1;"), 1);
}

// Values are OWS-trimmed on insert (RFC 7230 §3.2.4): leading/trailing
// spaces are stripped so `get` returns the bare value.
TEST(HttpHeadersTests, valuesAreOwsTrimmed) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"X-Pad\", \"   spaced   \");\n"
        "if (!h.get(\"x-pad\").equals(\"spaced\")) return -1;\n"
        "return 1;"), 1);
}

// A single occurrence of an ordinary header returns its lone value
// verbatim (no folding, no separator), even after the folding path
// would otherwise engage for >1 occurrence.
TEST(HttpHeadersTests, singleValueNotFolded) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"X-One\", \"solo\");\n"
        "if (!h.get(\"x-one\").equals(\"solo\")) return -1;\n"
        "return 1;"), 1);
}

// Growth: adding past the initial capacity (8) preserves order +
// values across the parallel-array grow().
TEST(HttpHeadersTests, growthPreservesEntries) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "int32 i = 0;\n"
        "while (i < 20) {\n"
        "    h.add(\"Multi\", \"v\");\n"
        "    i = i + 1;\n"
        "}\n"
        "if (h.count() != 20) return -1;\n"
        "if (h.getAll(\"multi\").count() != 20) return -2;\n"
        "if (!h.valueAt(19).equals(\"v\")) return -3;\n"
        "return 1;"), 1);
}

// remove() compacts with a FORWARDING move, and the survivors have to keep
// their bytes across an allocation.
//
// This is grow()'s bug in a second method. `grow` records it in its own
// comment — "a plain `=` left the old arrays owning their Strings, and the
// `#=` displacement below freed them through the element-drop walk
// (use-after-free …; json-grow-element-uaf)" — and `remove`'s compaction had
// the identical shape and the plain `=`.
//
// The CHURN is the whole test. removeCompactsPreservingOrder above reads the
// survivors immediately and passes either way: freed memory still holds the
// right bytes until something reuses it. Allocating between the compaction
// and the read is what makes a dangling slot observable, and without it a
// use-after-free reads as a clean run. Found through cajeta-http, where the
// ETag middleware's setHeader (= remove + add) handed the tour a header whose
// value pointer had become garbage (`uncaught exception (value=0x3)`).
TEST(HttpHeadersTests, removedSlotsSurviveAllocationChurn) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"A\", \"first\");\n"
        "h.add(\"B\", \"second\");\n"
        "h.add(\"C\", \"third\");\n"
        "h.add(\"D\", \"fourth\");\n"
        // Removes slot 0 and slides B, C, D down one — every survivor is
        // displaced, so every one of them is a candidate to dangle.
        "h.remove(\"a\");\n"
        // Churn: reuse whatever the compaction may have freed.
        "int32 i = 0;\n"
        "while (i < 64) {\n"
        "    Headers t = heap Headers();\n"
        "    t.add(\"Filler-Name-Long-Enough\", \"filler-value-long-enough\");\n"
        "    i = i + 1;\n"
        "}\n"
        "if (h.count() != 3) return -1;\n"
        "if (!h.valueAt(0).equals(\"second\")) return -2;\n"
        "if (!h.valueAt(1).equals(\"third\")) return -3;\n"
        "if (!h.valueAt(2).equals(\"fourth\")) return -4;\n"
        "if (!h.nameAt(0).equals(\"b\")) return -5;\n"
        "return 1;"), 1);
}

// The shape cajeta-http actually hit: set() on a map that already carries
// other headers is remove()+add(), so it compacts — and the value read back
// after further allocation must still be the one that was set.
TEST(HttpHeadersTests, setThenReadSurvivesAllocationChurn) {
    EXPECT_EQ(runI32(
        "Headers h = heap Headers();\n"
        "h.add(\"Content-Type\", \"application/json\");\n"
        "h.add(\"ETag\", \"\\\"stale\\\"\");\n"
        "h.add(\"Vary\", \"Accept-Encoding\");\n"
        "h.set(\"ETag\", \"\\\"0123456789abcdef\\\"\");\n"
        "int32 i = 0;\n"
        "while (i < 64) {\n"
        "    Headers t = heap Headers();\n"
        "    t.add(\"Filler-Name-Long-Enough\", \"filler-value-long-enough\");\n"
        "    i = i + 1;\n"
        "}\n"
        "String tag #= h.get(\"etag\");\n"
        "if (tag == null) return -1;\n"
        "if (!tag.equals(\"\\\"0123456789abcdef\\\"\")) return -2;\n"
        "if (!h.get(\"content-type\").equals(\"application/json\")) return -3;\n"
        "if (!h.get(\"vary\").equals(\"Accept-Encoding\")) return -4;\n"
        "return 1;"), 1);
}
