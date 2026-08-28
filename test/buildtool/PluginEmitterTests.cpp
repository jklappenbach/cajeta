// plugin-output-protocol plan §1.1 — the emitter, guarded.
//
// §1 shipped code-complete and UNTESTED (commit 1301d458, tests deferred by
// instruction). These are those tests. The property they exist to pin is the
// one that motivated the whole spec: `dev.cajeta.coverage` built records by
// string concatenation with no escaping, so a `"` or a newline in a message
// emitted malformed JSON, and the plugin looked broken for a reason nothing
// reported.
//
// ── ONE compile, many cases ──
//
// Every case here JIT-compiles the stdlib, so a file of N separate
// `CajetaJit::compile` calls costs N × ~18 s — this file was 4.6 minutes that
// way, in every sweep, forever. It is now one module compiled once in
// SetUpTestSuite (the ViewSafeConsumerTests / VectorTests / DropGapTests
// shape), with each check a `run_*` static returning 0 for pass and a DISTINCT
// non-zero code otherwise, so a failure still names which assertion broke.
// Add cases as methods on the module; do not add a second compile.
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
// So each check asserts the record CONTAINS the correct escape sequence and does
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
#include <memory>

using cajeta_test::CajetaJit;

namespace {

// A plain string literal binds to a `#String` formal. `#heap String("lit")`
// does NOT — String has no String(String) constructor, and that mistake cost
// the first run of this file 7 of its 15 cases.
const char* MODULE_SRC =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.buildtool.plugin.ActionContext;\n"
    "import cajeta.buildtool.plugin.Finding;\n"
    "import cajeta.buildtool.plugin.PluginHost;\n"
    "import cajeta.buildtool.plugin.RecordingActionContext;\n"
    "import cajeta.buildtool.plugin.Severity;\n"
    "import cajeta.collection.ArrayList;\n"
    "public final class D {\n"

    // ---- 1.1.1 — hostile strings survive the emitter ----
    //
    // Each of these characters breaks a concatenating emitter; each must appear
    // escaped, and the raw byte must not appear at all.

    // `a\"b` must serialize as a\\\"b. A raw quote closes the JSON string early
    // and the record does not parse at all.
    "    public static int32 run_quoteEscaped() {\n"
    "        String r #= PluginHost.logRecord(\"a\\\"b\", \"info\");\n"
    "        if (!r.contains(\"a\\\\\\\"b\")) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // A Windows path is the everyday case: C:\\x must not read as an escape.
    "    public static int32 run_backslashDoubled() {\n"
    "        String r #= PluginHost.logRecord(\"C:\\\\x\", \"info\");\n"
    "        if (!r.contains(\"C:\\\\\\\\x\")) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // The load-bearing one: a raw newline inside a JSON string is invalid
    // (RFC 8259 §7), and it also splits the NDJSON line in two, so the reader
    // sees a truncated record followed by garbage.
    "    public static int32 run_newlineEscaped() {\n"
    "        String r #= PluginHost.logRecord(\"a\\nb\", \"info\");\n"
    "        if (!r.contains(\"a\\\\nb\")) { return 1; }\n"
    "        if (r.contains(\"\\n\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    "    public static int32 run_tabEscaped() {\n"
    "        String r #= PluginHost.logRecord(\"a\\tb\", \"info\");\n"
    "        if (!r.contains(\"a\\\\tb\")) { return 1; }\n"
    "        if (r.contains(\"\\t\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // UTF-8 is valid in a JSON string unescaped; the emitter must pass it
    // through rather than mangling or dropping it.
    "    public static int32 run_nonAscii() {\n"
    "        String r #= PluginHost.logRecord(\"caf\\u00e9 \\u2014 na\\u00efve\", \"info\");\n"
    "        if (!r.contains(\"caf\\u00e9 \\u2014 na\\u00efve\")) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // The degenerate case a concatenating emitter usually gets right by
    // accident and a rewritten one can lose.
    "    public static int32 run_emptyMessage() {\n"
    "        String r #= PluginHost.logRecord(\"\", \"info\");\n"
    "        if (!r.contains(\"\\\"kind\\\":\\\"log\\\"\")) { return 1; }\n"
    "        if (!r.contains(\"\\\"message\\\":\\\"\\\"\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // ---- 1.1.2 — a located finding carries all three ----

    "    public static int32 run_locatedFinding() {\n"
    "        Finding f #= Finding.error(\"rule-x\", \"src/A.cajeta\", 12, 5, \"boom\");\n"
    "        String r #= PluginHost.findingRecord(f);\n"
    "        if (!r.contains(\"\\\"file\\\":\\\"src/A.cajeta\\\"\")) { return 1; }\n"
    "        if (!r.contains(\"\\\"line\\\":12\")) { return 2; }\n"
    "        if (!r.contains(\"\\\"column\\\":5\")) { return 3; }\n"
    "        return 0;\n"
    "    }\n"

    "    public static int32 run_severityAndRule() {\n"
    "        Finding f #= Finding.warning(\"drift\", \"src/A.cajeta\", 1, 1, \"m\");\n"
    "        String r #= PluginHost.findingRecord(f);\n"
    "        if (!r.contains(\"\\\"severity\\\":\\\"warning\\\"\")) { return 1; }\n"
    "        if (!r.contains(\"\\\"rule\\\":\\\"drift\\\"\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // ---- 1.1.3 — an unlocated finding fabricates nothing ----
    //
    // The negative arm, and the one that matters: a position of 0:0 navigates
    // SOMEWHERE, which is worse than navigating nowhere. Absence stays absence.

    "    public static int32 run_unlocatedFinding() {\n"
    "        Finding f #= Finding.info(\"rule-x\", \"\", 0, 0, \"no position\");\n"
    "        String r #= PluginHost.findingRecord(f);\n"
    "        if (r.contains(\"\\\"file\\\"\")) { return 1; }\n"
    "        if (r.contains(\"\\\"line\\\"\")) { return 2; }\n"
    "        if (r.contains(\"\\\"column\\\"\")) { return 3; }\n"
    "        if (!r.contains(\"\\\"message\\\":\\\"no position\\\"\")) { return 4; }\n"
    "        return 0;\n"
    "    }\n"

    // ---- 1.1.4 — an output value round-trips ----

    // Outputs are data the build consumes via ${id.key}; a raw newline here
    // corrupts the record exactly as it does in a message.
    "    public static int32 run_outputNewline() {\n"
    "        String r #= PluginHost.outputRecord(\"k\", \"line1\\nline2\");\n"
    "        if (!r.contains(\"line1\\\\nline2\")) { return 1; }\n"
    "        if (r.contains(\"\\n\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // Keys are as attacker-controlled as values, and a quote in a key breaks
    // the object rather than the string.
    "    public static int32 run_outputKeyEscaped() {\n"
    "        String r #= PluginHost.outputRecord(\"a\\\"k\", \"v\");\n"
    "        if (!r.contains(\"a\\\\\\\"k\")) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // ---- 1.1.6 — the recording context captures IN-PROCESS ----
    //
    // The property whose absence let the shipped API rot: before §1.4's
    // amendment the only channel was a static writer to STDOUT_FILENO,
    // observable solely through a subprocess, so nothing was ever asserted
    // about it. These run with no subprocess and no stdout capture. If these
    // are ever deleted, the API is unguarded again.

    "    public static int32 run_contextCaptures() {\n"
    "        RecordingActionContext ctx #= heap RecordingActionContext(\"/w\", \"proj\", \"1.0.0\");\n"
    "        ctx.log(\"hello\");\n"
    "        ctx.output(\"k\", \"v\");\n"
    "        if (ctx.count() != 2) { return 1; }\n"
    "        ArrayList<String> rs = ctx.records();\n"
    "        if (!rs.get(0).contains(\"\\\"kind\\\":\\\"log\\\"\")) { return 2; }\n"
    "        if (!rs.get(1).contains(\"\\\"kind\\\":\\\"output\\\"\")) { return 3; }\n"
    "        return 0;\n"
    "    }\n"

    // The control. A capture that starts non-empty would make every count
    // assertion above meaningless.
    "    public static int32 run_freshContextIsEmpty() {\n"
    "        RecordingActionContext ctx #= heap RecordingActionContext(\"/w\", \"proj\", \"1.0.0\");\n"
    "        if (ctx.count() != 0) { return 1; }\n"
    "        return 0;\n"
    "    }\n"

    // The context is the path a real plugin takes. Testing PluginHost alone
    // would leave the actual call site unguarded.
    "    public static int32 run_contextEscaping() {\n"
    "        RecordingActionContext ctx #= heap RecordingActionContext(\"/w\", \"proj\", \"1.0.0\");\n"
    "        ctx.log(\"unknown type \\\"Runner\\\" at C:\\\\x\");\n"
    "        String r = ctx.records().get(0);\n"
    "        if (!r.contains(\"\\\\\\\"Runner\\\\\\\"\")) { return 1; }\n"
    "        if (!r.contains(\"C:\\\\\\\\x\")) { return 2; }\n"
    "        return 0;\n"
    "    }\n"

    // Guards the one unexplained observation in §1's pick-up state: a single
    // probe run showed a stray `1` appended to an output record, and four
    // re-runs did not reproduce it. If that was real rather than the probe's
    // own concatenation, a record stored before a Finding is where it would
    // show. It has never reproduced here either — guarded, not explained.
    "    public static int32 run_findingMidStream() {\n"
    "        RecordingActionContext ctx #= heap RecordingActionContext(\"/w\", \"proj\", \"1.0.0\");\n"
    "        ctx.output(\"first\", \"v1\");\n"
    "        Finding f #= Finding.error(\"r\", \"f\", 1, 1, \"m\");\n"
    "        ctx.finding(f);\n"
    "        ctx.output(\"third\", \"v3\");\n"
    "        if (ctx.count() != 3) { return 1; }\n"
    "        ArrayList<String> rs = ctx.records();\n"
    "        if (!rs.get(0).contains(\"\\\"value\\\":\\\"v1\\\"\")) { return 2; }\n"
    "        if (!rs.get(2).contains(\"\\\"value\\\":\\\"v3\\\"\")) { return 3; }\n"
    "        return 0;\n"
    "    }\n"

    "}\n";

class PluginEmitterTests : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        jit = CajetaJit::compile(MODULE_SRC, "test.D");
    }
    static void TearDownTestSuite() {
        jit.reset();
    }
    static int32_t i32(const char* name) {
        auto fn = jit->lookup<int32_t (*)()>(name);
        return fn();
    }
    static std::unique_ptr<CajetaJit> jit;
};

std::unique_ptr<CajetaJit> PluginEmitterTests::jit;

}  // namespace

// 1.1.1 — hostile strings
TEST_F(PluginEmitterTests, quoteEscaped)        { EXPECT_EQ(i32("run_quoteEscaped"), 0); }
TEST_F(PluginEmitterTests, backslashDoubled)    { EXPECT_EQ(i32("run_backslashDoubled"), 0); }
TEST_F(PluginEmitterTests, newlineEscaped)      { EXPECT_EQ(i32("run_newlineEscaped"), 0); }
TEST_F(PluginEmitterTests, tabEscaped)          { EXPECT_EQ(i32("run_tabEscaped"), 0); }
TEST_F(PluginEmitterTests, nonAscii)            { EXPECT_EQ(i32("run_nonAscii"), 0); }
TEST_F(PluginEmitterTests, emptyMessage)        { EXPECT_EQ(i32("run_emptyMessage"), 0); }

// 1.1.2 / 1.1.3 — findings, located and not
TEST_F(PluginEmitterTests, locatedFinding)      { EXPECT_EQ(i32("run_locatedFinding"), 0); }
TEST_F(PluginEmitterTests, severityAndRule)     { EXPECT_EQ(i32("run_severityAndRule"), 0); }
TEST_F(PluginEmitterTests, unlocatedFinding)    { EXPECT_EQ(i32("run_unlocatedFinding"), 0); }

// 1.1.4 — outputs
TEST_F(PluginEmitterTests, outputNewline)       { EXPECT_EQ(i32("run_outputNewline"), 0); }
TEST_F(PluginEmitterTests, outputKeyEscaped)    { EXPECT_EQ(i32("run_outputKeyEscaped"), 0); }

// 1.1.6 — in-process capture
TEST_F(PluginEmitterTests, contextCaptures)     { EXPECT_EQ(i32("run_contextCaptures"), 0); }
TEST_F(PluginEmitterTests, freshContextIsEmpty) { EXPECT_EQ(i32("run_freshContextIsEmpty"), 0); }
TEST_F(PluginEmitterTests, contextEscaping)     { EXPECT_EQ(i32("run_contextEscaping"), 0); }
TEST_F(PluginEmitterTests, findingMidStream)    { EXPECT_EQ(i32("run_findingMidStream"), 0); }

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
//      offset.
//   2. §3 has not wired the validator into `PluginRuntime`. Until it does, the
//      running path still reads fields ad-hoc with `getString`, so there is not
//      yet ONE reader to test against — which is plan §0.3.2's open `[~]` and
//      exactly what §3 closes.
//
// Enable with §3, not before.

TEST(PluginEmitterConformance, DISABLED_everyRecordKindPassesTheConformanceSuite) {
    FAIL() << "1.1.5/1.1.7: needs the §0 validator wired into PluginRuntime (§3) "
              "and the int8[] JIT-boundary crossing pinned by a real run.";
}
