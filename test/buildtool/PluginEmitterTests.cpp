// plugin-output-protocol plan §1.1 — the emitter, guarded.
//
// §1 shipped code-complete and UNTESTED (commit 1301d458, tests deferred by
// instruction). These are those tests. The property they exist to pin is the
// one that motivated the whole spec: `dev.cajeta.coverage` built records by
// string concatenation with no escaping, so a `"` or a newline in a message
// emitted malformed JSON, and the plugin looked broken for a reason nothing
// reported.
//
// ── Why the assertions are on the ESCAPED BYTES, not on a parsed value ──
//
// The obvious test — emit, parse, compare to the original — is WRONG here, and
// wrong in a way that would look like an emitter defect:
//
//     JsonValue.asString()      returns escapes VERBATIM      (JsonValue:152)
//     JsonObject.getString(k)   delegates to asString()       (JsonObject:330)
//
// so a message containing a newline parses back as the two characters `\` `n`,
// never as U+000A. Comparing that to the original fails on exactly the case §1
// exists to fix. Decoding needs `JsonReader.currentDecodedString()`
// (JsonReader:751), a SAX walk this file does not need.
//
// So each test asserts the record CONTAINS the correct escape sequence and does
// NOT contain the raw control byte. That is the emitter's actual contract, and
// it cannot be satisfied by the concatenating emitter the spec replaced.
//
// Six cajeta-llama tests once looked like engine bugs and were this trap.
// If one of these fails, check the reader before touching PluginHost.
//
// ── What is NOT covered here ──
//
// 1.1.5 and 1.1.7 want the records read by the BUILD TOOL's own reader (the C++
// `checkPluginRecord`, plan §0) rather than by the stdlib's. That needs the
// record bytes to cross the JIT boundary into C++, and §3 has not yet wired the
// validator into `PluginRuntime` — until it does, the running path still reads
// fields ad-hoc with `getString`, so "the build tool's reader" is not one thing
// to test against. Recorded as a DISABLED test at the bottom rather than faked
// against a fixture, which 1.1.5 explicitly rules out.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.P");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Wrap a method body in a class importing the plugin API. The body must
// `return` an int32 — 1 for pass, 0 for fail, so the assertion runs in cajeta
// where the values live and no string has to cross the ABI.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.buildtool.plugin.ActionContext;\n"
           "import cajeta.buildtool.plugin.Finding;\n"
           "import cajeta.buildtool.plugin.PluginHost;\n"
           "import cajeta.buildtool.plugin.RecordingActionContext;\n"
           "import cajeta.buildtool.plugin.Severity;\n"
           "import cajeta.collection.ArrayList;\n"
           "public final class P {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// ---- 1.1.1 — hostile strings survive the emitter -----------------------------
//
// The defect that motivated the spec. Each of these characters breaks a
// concatenating emitter; each must appear in the record in its ESCAPED form,
// and the raw byte must not appear at all.

TEST(PluginEmitterTests, aQuoteIsEscapedAndTheRawByteIsGone) {
    // `a"b` must serialize as a\"b. A raw quote would close the JSON string
    // early and the record would not parse at all.
    EXPECT_EQ(runI32(makeSource(
        "String r #= PluginHost.logRecord(\"a\\\"b\", \"info\");\n"
        "if (!r.contains(\"a\\\\\\\"b\")) { return 0; }\n"
        "return 1;")), 1);
}

TEST(PluginEmitterTests, aBackslashIsDoubled) {
    // A Windows path is the everyday case: C:\x must not read as an escape.
    EXPECT_EQ(runI32(makeSource(
        "String r #= PluginHost.logRecord(\"C:\\\\x\", \"info\");\n"
        "return r.contains(\"C:\\\\\\\\x\") ? 1 : 0;")), 1);
}

TEST(PluginEmitterTests, aNewlineBecomesTwoCharactersAndNeverRidesRaw) {
    // The load-bearing one: a raw newline inside a JSON string is invalid
    // (RFC 8259 §7), and it also splits the NDJSON line in two, so the reader
    // sees a truncated record followed by garbage.
    EXPECT_EQ(runI32(makeSource(
        "String r #= PluginHost.logRecord(\"a\\nb\", \"info\");\n"
        "if (!r.contains(\"a\\\\nb\")) { return 0; }\n"
        "return r.contains(\"\\n\") ? 0 : 1;")), 1);
}

TEST(PluginEmitterTests, aTabIsEscaped) {
    EXPECT_EQ(runI32(makeSource(
        "String r #= PluginHost.logRecord(\"a\\tb\", \"info\");\n"
        "if (!r.contains(\"a\\\\tb\")) { return 0; }\n"
        "return r.contains(\"\\t\") ? 0 : 1;")), 1);
}

TEST(PluginEmitterTests, nonAsciiSurvivesAsItsOwnBytes) {
    // UTF-8 is valid in a JSON string unescaped; the emitter must pass it
    // through rather than mangling or dropping it.
    EXPECT_EQ(runI32(makeSource(
        "String r #= PluginHost.logRecord(\"caf\\u00e9 \\u2014 na\\u00efve\", \"info\");\n"
        "return r.contains(\"caf\\u00e9 \\u2014 na\\u00efve\") ? 1 : 0;")), 1);
}

TEST(PluginEmitterTests, anEmptyMessageStillEmitsAWellFormedRecord) {
    // The degenerate case a concatenating emitter usually gets right by
    // accident and a rewritten one can lose.
    EXPECT_EQ(runI32(makeSource(
        "String r #= PluginHost.logRecord(\"\", \"info\");\n"
        "if (!r.contains(\"\\\"kind\\\":\\\"log\\\"\")) { return 0; }\n"
        "return r.contains(\"\\\"message\\\":\\\"\\\"\") ? 1 : 0;")), 1);
}

// ---- 1.1.2 — a located finding carries all three ------------------------------

TEST(PluginEmitterTests, aLocatedFindingCarriesFileLineAndColumn) {
    EXPECT_EQ(runI32(makeSource(
        "Finding f #= Finding.error(\"rule-x\", "
        "\"src/A.cajeta\", 12, 5, \"boom\");\n"
        "String r #= PluginHost.findingRecord(f);\n"
        "if (!r.contains(\"\\\"file\\\":\\\"src/A.cajeta\\\"\")) { return 0; }\n"
        "if (!r.contains(\"\\\"line\\\":12\")) { return 0; }\n"
        "return r.contains(\"\\\"column\\\":5\") ? 1 : 0;")), 1);
}

TEST(PluginEmitterTests, aFindingCarriesItsSeverityAndRule) {
    EXPECT_EQ(runI32(makeSource(
        "Finding f #= Finding.warning(\"drift\", "
        "\"src/A.cajeta\", 1, 1, \"m\");\n"
        "String r #= PluginHost.findingRecord(f);\n"
        "if (!r.contains(\"\\\"severity\\\":\\\"warning\\\"\")) { return 0; }\n"
        "return r.contains(\"\\\"rule\\\":\\\"drift\\\"\") ? 1 : 0;")), 1);
}

// ---- 1.1.3 — an unlocated finding fabricates nothing --------------------------
//
// The negative arm, and the one that matters: a position of 0:0 navigates
// SOMEWHERE, which is worse than navigating nowhere. Absence must stay absence.

TEST(PluginEmitterTests, anUnlocatedFindingEmitsNoLocationKeysAtAll) {
    EXPECT_EQ(runI32(makeSource(
        "Finding f #= Finding.info(\"rule-x\", "
        "\"\", 0, 0, \"no position\");\n"
        "String r #= PluginHost.findingRecord(f);\n"
        "if (r.contains(\"\\\"file\\\"\")) { return 0; }\n"
        "if (r.contains(\"\\\"line\\\"\")) { return 0; }\n"
        "if (r.contains(\"\\\"column\\\"\")) { return 0; }\n"
        "return r.contains(\"\\\"message\\\":\\\"no position\\\"\") ? 1 : 0;")), 1);
}

// ---- 1.1.4 — an output value round-trips --------------------------------------

TEST(PluginEmitterTests, anOutputValueWithANewlineIsEscaped) {
    // Outputs are data the build consumes via ${id.key}; a raw newline here
    // corrupts the record exactly as it does in a message.
    EXPECT_EQ(runI32(makeSource(
        "String r #= PluginHost.outputRecord(\"k\", \"line1\\nline2\");\n"
        "if (!r.contains(\"line1\\\\nline2\")) { return 0; }\n"
        "return r.contains(\"\\n\") ? 0 : 1;")), 1);
}

TEST(PluginEmitterTests, anOutputKeyIsEscapedToo) {
    // Keys are as attacker-controlled as values, and a quote in a key breaks
    // the object rather than the string.
    EXPECT_EQ(runI32(makeSource(
        "String r #= PluginHost.outputRecord(\"a\\\"k\", \"v\");\n"
        "return r.contains(\"a\\\\\\\"k\") ? 1 : 0;")), 1);
}

// ---- 1.1.6 — the recording context captures IN-PROCESS -------------------------
//
// The property whose absence let the shipped API rot: before §1.4's amendment
// the only channel was a static writer to STDOUT_FILENO, observable solely
// through a subprocess, so nothing was ever asserted about it. These run with
// no subprocess and no stdout capture. This is the entire reason the amendment
// exists — if this test is ever deleted, the API is unguarded again.

TEST(PluginEmitterTests, aRecordingContextCapturesWhatAnActionEmitted) {
    EXPECT_EQ(runI32(makeSource(
        "RecordingActionContext ctx #= heap RecordingActionContext("
        "\"/w\", \"proj\", \"1.0.0\");\n"
        "ctx.log(\"hello\");\n"
        "ctx.output(\"k\", \"v\");\n"
        "if (ctx.count() != 2) { return 0; }\n"
        "ArrayList<String> rs = ctx.records();\n"
        "if (!rs.get(0).contains(\"\\\"kind\\\":\\\"log\\\"\")) { return 0; }\n"
        "return rs.get(1).contains(\"\\\"kind\\\":\\\"output\\\"\") ? 1 : 0;")), 1);
}

TEST(PluginEmitterTests, aFreshRecordingContextHasCapturedNothing) {
    // The control. A capture that starts non-empty would make every count
    // assertion above meaningless.
    EXPECT_EQ(runI32(makeSource(
        "RecordingActionContext ctx #= heap RecordingActionContext("
        "\"/w\", \"proj\", \"1.0.0\");\n"
        "return ctx.count() == 0 ? 1 : 0;")), 1);
}

TEST(PluginEmitterTests, escapingHoldsThroughTheContextNotJustTheSerializer) {
    // The context is the path a real plugin takes. Testing PluginHost alone
    // would leave the actual call site unguarded.
    EXPECT_EQ(runI32(makeSource(
        "RecordingActionContext ctx #= heap RecordingActionContext("
        "\"/w\", \"proj\", \"1.0.0\");\n"
        "ctx.log(\"unknown type \\\"Runner\\\" at C:\\\\x\");\n"
        "String r = ctx.records().get(0);\n"
        "if (!r.contains(\"\\\\\\\"Runner\\\\\\\"\")) { return 0; }\n"
        "return r.contains(\"C:\\\\\\\\x\") ? 1 : 0;")), 1);
}

TEST(PluginEmitterTests, aFindingInsertedMidStreamDisturbsNoStoredRecord) {
    // Guards the one unexplained observation in §1's pick-up state: a single
    // probe run showed a stray `1` appended to an output record, and four
    // re-runs did not reproduce it. If that was real rather than the probe's
    // own concatenation, a record stored before a Finding is where it would
    // show, so this is the shape that would catch it.
    EXPECT_EQ(runI32(makeSource(
        "RecordingActionContext ctx #= heap RecordingActionContext("
        "\"/w\", \"proj\", \"1.0.0\");\n"
        "ctx.output(\"first\", \"v1\");\n"
        "Finding f #= Finding.error(\"r\", \"f\", "
        "1, 1, \"m\");\n"
        "ctx.finding(f);\n"
        "ctx.output(\"third\", \"v3\");\n"
        "if (ctx.count() != 3) { return 0; }\n"
        "ArrayList<String> rs = ctx.records();\n"
        "if (!rs.get(0).contains(\"\\\"value\\\":\\\"v1\\\"\")) { return 0; }\n"
        "return rs.get(2).contains(\"\\\"value\\\":\\\"v3\\\"\") ? 1 : 0;")), 1);
}

// ---- 1.1.5 / 1.1.7 — the build tool's own reader --------------------------------
//
// DISABLED deliberately, not skipped by accident.
//
// These want the emitted records validated by `checkPluginRecord` (plan §0) —
// producer and consumer tested against each other rather than against a fixture
// either could drift from. Two things block it, and neither is worth faking:
//
//   1. The record bytes must cross the JIT boundary into C++. A cajeta static
//      returning `#int8[]` hands back an array HEADER with the payload at +8;
//      that ABI is real but has to be exercised once to pin the length field's
//      offset, and this file cannot do that without running.
//   2. §3 has not wired the validator into `PluginRuntime`. Until it does, the
//      running path still reads fields ad-hoc with `getString`, so there is not
//      yet ONE reader to test against — which is plan §0.3.2's open `[~]` and
//      exactly what §3 closes.
//
// Enable with §3, not before.

TEST(PluginEmitterTests, DISABLED_everyRecordKindPassesTheConformanceSuite) {
    FAIL() << "1.1.5/1.1.7: needs the §0 validator wired into PluginRuntime (§3) "
              "and the int8[] JIT-boundary crossing pinned by a real run.";
}
