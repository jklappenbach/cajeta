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
// ── Two levels of assertion, on purpose ──
//
// The §1.1 cases below assert the ESCAPE BYTES. The §1.3 cases added later
// assert a DECODING ROUND-TRIP — the record re-parses to the original value,
// via `currentDecodedString()`. Escaping correctly and decoding back to what
// went in are two different claims, and only the second is what a consumer
// depends on.
//
// The last group crosses the JIT boundary and hands the bytes to the BUILD
// TOOL's own reader (`checkPluginStream` / `checkPluginRecord`, plan §0, and
// `parseJsonC`, which is what `PluginRuntime` actually parses with). That is
// producer and consumer tested against each other rather than each against a
// fixture either could drift from — 1.1.5, 1.1.7, 1.3.3 and 1.3.4.
//
// ── A trap that cost a wrong bug report ──
//
// A `jit-run` probe uses `build/src/cajeta`, which a test sweep does NOT
// rebuild. On 2026-08-28 that binary was four days stale and predated
// `bc825d85` (JsonWriter escaping control characters), so a probe showed raw
// newlines in a record and looked exactly like an emitter defect. The stdlib
// is embedded: `ninja -C build cajeta` before believing a probe.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include "cajeta/buildtool/JsonC.h"
#include "cajeta/buildtool/PluginRecord.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

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
    "import cajeta.codec.json.JsonReader;\n"
    "import cajeta.codec.json.JsonToken;\n"
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


    // ---- 1.3.1 — a decoding round-trip, not an escape-shape check ----
    //
    // The tests above assert the ESCAPE BYTES because that is cheap and it is the
    // emitter's contract. 1.3.1 asks for something stronger and different: that a
    // record re-parses to the ORIGINAL values. Escaping correctly and decoding
    // back to what went in are two claims, and only the second one is what a
    // consumer actually depends on.
    //
    // Decoding needs `currentDecodedString()` — `asString()`/`getString()` return
    // escapes VERBATIM (see the header). This is that SAX walk, done once here so
    // every kind can be checked through it.

    "static boolean fieldEquals(String rec, String key, String expected) {\n"
    "    int8[] b #= rec.toBytes();\n"
    "    JsonReader r #= heap JsonReader(b, (int64) rec.byteLength());\n"
    "    JsonToken t = r.next();\n"
    "    while (t != JsonToken.END) {\n"
    "        if (t == JsonToken.KEY) {\n"
    "            String k #= r.currentDecodedString();\n"
    "            t = r.next();\n"
    "            if (t == JsonToken.STRING) {\n"
    "                if (k.equals(key)) {\n"
    "                    String v #= r.currentDecodedString();\n"
    "                    return v.equals(expected);\n"
    "                }\n"
    "            }\n"
    "        } else {\n"
    "            t = r.next();\n"
    "        }\n"
    "    }\n"
    "    return false;\n"
    "}\n"

    "static boolean hasKey(String rec, String key) {\n"
    "    int8[] b #= rec.toBytes();\n"
    "    JsonReader r #= heap JsonReader(b, (int64) rec.byteLength());\n"
    "    JsonToken t = r.next();\n"
    "    while (t != JsonToken.END) {\n"
    "        if (t == JsonToken.KEY) {\n"
    "            String k #= r.currentDecodedString();\n"
    "            if (k.equals(key)) { return true; }\n"
    "        }\n"
    "        t = r.next();\n"
    "    }\n"
    "    return false;\n"
    "}\n"

    // 0 quote, 1 backslash, 2 newline, 3 tab, 4 non-ASCII, 5 empty. The failure
    // codes below are base+index, so a red test names the character as well as
    // the field.
    "static #ArrayList<String> hostiles() {\n"
    "    ArrayList<String> a #= heap ArrayList<String>();\n"
    "    a.add(\"a\\\"b\");\n"
    "    a.add(\"C:\\\\x\");\n"
    "    a.add(\"a\\nb\");\n"
    "    a.add(\"a\\tb\");\n"
    "    a.add(\"café — naïve\");\n"
    "    a.add(\"\");\n"
    "    return #a;\n"
    "}\n"

    "public static int32 run_logRoundTrip() {\n"
    "    ArrayList<String> hs #= D.hostiles();\n"
    "    int32 i = 0;\n"
    "    while (i < hs.count()) {\n"
    "        String h = hs.get(i);\n"
    "        String r #= PluginHost.logRecord(h, \"info\");\n"
    "        if (!D.fieldEquals(r, \"message\", h)) { return 10 + i; }\n"
    "        if (!D.fieldEquals(r, \"level\", \"info\")) { return 20 + i; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"

    "public static int32 run_warnRoundTrip() {\n"
    "    ArrayList<String> hs #= D.hostiles();\n"
    "    int32 i = 0;\n"
    "    while (i < hs.count()) {\n"
    "        String h = hs.get(i);\n"
    "        String r #= PluginHost.warnRecord(h);\n"
    "        if (!D.fieldEquals(r, \"message\", h)) { return 10 + i; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"

    "public static int32 run_writeRoundTrip() {\n"
    "    ArrayList<String> hs #= D.hostiles();\n"
    "    int32 i = 0;\n"
    "    while (i < hs.count()) {\n"
    "        String h = hs.get(i);\n"
    "        String r #= PluginHost.writeRecord(h);\n"
    "        if (!D.fieldEquals(r, \"text\", h)) { return 10 + i; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"

    // Keys are as plugin-controlled as values, so both sides are hostile here.
    "public static int32 run_outputRoundTrip() {\n"
    "    ArrayList<String> hs #= D.hostiles();\n"
    "    int32 i = 0;\n"
    "    while (i < hs.count()) {\n"
    "        String h = hs.get(i);\n"
    "        String r #= PluginHost.outputRecord(h, h);\n"
    "        if (!D.fieldEquals(r, \"key\", h))   { return 10 + i; }\n"
    "        if (!D.fieldEquals(r, \"value\", h)) { return 20 + i; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"

    // message for every hostile string; rule and file only where non-empty,
    // since an empty rule or file means ABSENT and is asserted elsewhere.
    "public static int32 run_findingRoundTrip() {\n"
    "    ArrayList<String> hs #= D.hostiles();\n"
    "    int32 i = 0;\n"
    "    while (i < hs.count()) {\n"
    "        String h = hs.get(i);\n"
    "        Finding f #= Finding.error(\"r\", \"f\", 1, 1, h);\n"
    "        String r #= PluginHost.findingRecord(f);\n"
    "        if (!D.fieldEquals(r, \"message\", h)) { return 10 + i; }\n"
    "        if (h.byteLength() > 0) {\n"
    "            Finding g #= Finding.warning(h, h, 4, 9, \"m\");\n"
    "            String q #= PluginHost.findingRecord(g);\n"
    "            if (!D.fieldEquals(q, \"rule\", h)) { return 20 + i; }\n"
    "            if (!D.fieldEquals(q, \"file\", h)) { return 30 + i; }\n"
    "        }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return 0;\n"
    "}\n"

    "public static int32 run_resultRoundTrip() {\n"
    "    ArrayList<String> hs #= D.hostiles();\n"
    "    int32 i = 0;\n"
    "    while (i < hs.count()) {\n"
    "        String h = hs.get(i);\n"
    "        String r #= PluginHost.errorResultRecord(h);\n"
    "        if (!D.fieldEquals(r, \"message\", h)) { return 10 + i; }\n"
    "        if (!D.fieldEquals(r, \"status\", \"error\")) { return 20 + i; }\n"
    "        i = i + 1;\n"
    "    }\n"
    "    String ok #= PluginHost.okResultRecord();\n"
    "    if (!D.fieldEquals(ok, \"status\", \"ok\")) { return 30; }\n"
        // An ok result carries no message. A fabricated empty one would read as a
        // message the plugin never set.
    "    if (D.hasKey(ok, \"message\")) { return 31; }\n"
    "    return 0;\n"
    "}\n"

    // The control. A decoder that returned true for everything — or that never
    // found a key and fell through to `false` on the negative cases only by
    // accident — would make every round-trip above vacuous.
    "public static int32 run_decodeIsNotVacuous() {\n"
    "    String r #= PluginHost.logRecord(\"a\\nb\", \"info\");\n"
    "    if (D.fieldEquals(r, \"message\", \"a\\\\nb\"))   { return 1; }\n"
    "    if (D.fieldEquals(r, \"message\", \"wrong\"))   { return 2; }\n"
    "    if (D.fieldEquals(r, \"nosuchkey\", \"a\\nb\"))  { return 3; }\n"
    "    if (D.hasKey(r, \"nosuchkey\"))               { return 4; }\n"
    "    if (!D.fieldEquals(r, \"message\", \"a\\nb\"))   { return 5; }\n"
    "    if (!D.hasKey(r, \"message\"))                { return 6; }\n"
    "    return 0;\n"
    "}\n"

    // ---- 1.3.3 / 1.3.4 — the records cross into C++ and face the build tool ----
    //
    // Everything above is the stdlib checking itself. These bytes go to the
    // CONSUMER: `checkPluginStream` (plan §0), the build tool's own definition of
    // a valid record. Producer and consumer meet here rather than each being
    // compared to a fixture that either could drift from.
    //
    // An action written on the SHIPPED API — `ActionContext` only, no `PluginHost`
    // call — so what is validated is what a plugin author can actually write.
    // Every string is hostile, because a conformance pass on tame input proves
    // nothing about the case the spec exists for.
    "static void emitEveryKind(ActionContext ctx) {\n"
    "    ctx.log(\"log \\\"quoted\\\"\\nsecond line\");\n"
    "    ctx.warn(\"warn\\twith a tab\");\n"
    "    ctx.write(\"write C:\\\\path — café\");\n"
    "    ctx.output(\"key\\\"with quote\", \"value\\nwith newline\");\n"
    "    Finding located #= Finding.error(\"rule\\\"r\", \"src/A\\n.cajeta\", 7, 3, \"msg\\ttab\");\n"
    "    ctx.finding(located);\n"
    "    Finding bare #= Finding.info(\"\", \"\", 0, 0, \"no position at all\");\n"
    "    ctx.finding(bare);\n"
    "}\n"

    // The records as NDJSON, exactly the shape the runtime reads off a plugin's
    // stdout. Returned as `#int8[]`, which crosses to C++ as the array HEADER:
    // `{ i64 count, [N x i8] data }` — count at +0, payload at +8.
    "public static #int8[] run_everyKindThroughTheApi() {\n"
    "    RecordingActionContext ctx #= heap RecordingActionContext(\"/w\", \"proj\", \"1.0.0\");\n"
    "    D.emitEveryKind(ctx);\n"
    "    ArrayList<String> rs = ctx.records();\n"
    "    String all = \"\";\n"
    "    int32 i = 0;\n"
    "    while (i < rs.count()) {\n"
    "        all = all + rs.get(i) + \"\\n\";\n"
    "        i = i + 1;\n"
    "    }\n"
    "    return all.toBytes();\n"
    "}\n"


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

    // A cajeta `#int8[]` return crosses to C++ as the ARRAY HEADER —
    // `{ i64 count, [N x i8] data }` — so the count is at +0 and the payload
    // at +8. Reading the header pointer as bytes would take the count word as
    // data; the classic signature is a byte stream that begins with the length.
    // Precedent: `__cajeta_sha1_update` (runtime/native/cajeta_sha1.c).
    static std::vector<std::string> ndjson(const char* name) {
        auto fn = jit->lookup<void* (*)()>(name);
        const void* hdr = fn();
        if (hdr == nullptr) return {};
        const auto count = *reinterpret_cast<const int64_t*>(hdr);
        const char* payload = reinterpret_cast<const char*>(hdr) + 8;
        const std::string all(payload, static_cast<size_t>(count));

        std::vector<std::string> lines;
        size_t start = 0;
        while (start < all.size()) {
            size_t nl = all.find('\n', start);
            if (nl == std::string::npos) nl = all.size();
            if (nl > start) lines.push_back(all.substr(start, nl - start));
            start = nl + 1;
        }
        return lines;
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
// 1.3.1 — the round-trip: every record kind re-parses to the ORIGINAL value
//
// Six hostile strings (quote, backslash, newline, tab, non-ASCII, empty)
// through every field of every kind. Failure codes are base+index, so a red
// test names the character as well as the field.

TEST_F(PluginEmitterTests, logRoundTrip)     { EXPECT_EQ(i32("run_logRoundTrip"), 0); }
TEST_F(PluginEmitterTests, warnRoundTrip)    { EXPECT_EQ(i32("run_warnRoundTrip"), 0); }
TEST_F(PluginEmitterTests, writeRoundTrip)   { EXPECT_EQ(i32("run_writeRoundTrip"), 0); }
TEST_F(PluginEmitterTests, outputRoundTrip)  { EXPECT_EQ(i32("run_outputRoundTrip"), 0); }
TEST_F(PluginEmitterTests, findingRoundTrip) { EXPECT_EQ(i32("run_findingRoundTrip"), 0); }
TEST_F(PluginEmitterTests, resultRoundTrip)  { EXPECT_EQ(i32("run_resultRoundTrip"), 0); }

// The negative arm. A decoder that returned true for everything would make
// all six of the above vacuous, and a decode test that only ever passes is
// indistinguishable from one that never ran.
TEST_F(PluginEmitterTests, decodeIsNotVacuous) {
    EXPECT_EQ(i32("run_decodeIsNotVacuous"), 0);
}

// 1.1.7 — records from the SHIPPED API against §0's conformance suite
//
// Emitted through `ActionContext` alone — no `PluginHost` call — so what is
// validated is what a plugin author can actually write. Every string is
// hostile: conformance on tame input proves nothing about the case the spec
// exists for.
//
// ── 1.3.3 is BLOCKED by this test, and the block is the point ──
//
// A plugin written entirely on the shipped API cannot pass §0's suite today.
// Not because a record is wrong — every one of them is valid — but because
// the suite requires exactly one `result` record and `ActionContext` has no
// way to emit one. `PluginHost` can build both result records; nothing
// reaches them.
//
// So the assertion is the precise shape of the gap: every record conforms,
// and the ONLY complaint is the missing result. §2 gives the context a
// result, and this test then flips to `report.passed` with no problems at
// all — which is what closes plan 1.3.3 and 2.3.3.
//
// Asserting the exact problem rather than skipping the test keeps the gap
// measured. A DISABLED test here would say "conformance is untested"; this
// says "conformance fails in exactly one known way, and here it is."

TEST_F(PluginEmitterTests, theOnlyConformanceGapIsTheResultTheApiCannotEmitYet) {
    const auto lines = ndjson("run_everyKindThroughTheApi");
    ASSERT_EQ(lines.size(), 6u) << "the action emits six records";

    const auto report = cajeta::buildtool::checkPluginStream(lines);

    std::string why;
    for (const auto& p : report.problems) why += "\n  " + p;

    // Every RECORD is conformant. Anything else in this list is a real
    // regression in the emitter and must fail here.
    ASSERT_EQ(report.problems.size(), 1u)
        << "expected exactly one gap (the missing result), got:" << why;
    EXPECT_EQ(report.problems[0],
              "no result record: the action never reported how it finished")
        << "the emitter has a conformance problem beyond the known §2 gap:" << why;
    EXPECT_FALSE(report.passed) << "if this now passes, §2 has landed — close"
                                  " plan 1.3.3 / 2.3.3 and assert passed==true";
}

// 1.1.5 — and under the parser the RUNTIME actually uses
//
// `checkPluginStream` parses with `llvm::json::parse`; `PluginRuntime` parses
// with `parseJsonC`. Two readers today — §3 unifies them (plan §0.3.2). Until
// it does, passing only one of them would leave the running path unproven, so
// the records face both.
TEST_F(PluginEmitterTests, everyKindParsesUnderTheRuntimesReader) {
    const auto lines = ndjson("run_everyKindThroughTheApi");
    ASSERT_FALSE(lines.empty());

    for (const auto& line : lines) {
        auto parsed = cajeta::buildtool::parseJsonC(line);
        if (!parsed) {
            ADD_FAILURE() << "PluginRuntime's parser rejects: " << line << "\n  "
                          << llvm::toString(parsed.takeError());
            continue;
        }
        const llvm::json::Object* obj = parsed->getAsObject();
        ASSERT_NE(obj, nullptr) << "not a JSON object: " << line;

        const auto check = cajeta::buildtool::checkPluginRecord(*obj);
        EXPECT_EQ(check.verdict, cajeta::buildtool::RecordVerdict::Valid)
            << check.reason << " in: " << line;
    }
}

// 1.3.4 — the implementation and the consumer agree on the vocabulary
//
// `ActionContext` shipped as an interface with no implementation and no
// consumer, so nothing connected the two ends. This pins them: every kind the
// API can emit is a kind the build tool KNOWS, not merely one it tolerates.
// `checkPluginStream` passes an unknown kind on purpose (forward
// compatibility), so a test that only checked conformance would stay green
// while the two ends drifted apart.
//
// `result` is absent by design — the context cannot emit one yet. §2 adds it,
// and this assertion is what will require the consumer to be taught about it.
TEST_F(PluginEmitterTests, theShippedApiEmitsOnlyKindsTheBuildToolKnows) {
    const auto lines = ndjson("run_everyKindThroughTheApi");
    ASSERT_FALSE(lines.empty());

    std::set<std::string> kinds;
    for (const auto& line : lines) {
        auto parsed = cajeta::buildtool::parseJsonC(line);
        ASSERT_TRUE(!!parsed) << line;
        const llvm::json::Object* obj = parsed->getAsObject();
        ASSERT_NE(obj, nullptr);

        const auto kind = obj->getString("kind");
        ASSERT_TRUE(kind.has_value()) << "record has no kind: " << line;
        kinds.insert(kind->str());

        EXPECT_NE(cajeta::buildtool::checkPluginRecord(*obj).verdict,
                  cajeta::buildtool::RecordVerdict::UnknownKind)
            << "the API emits a kind the build tool does not know: " << line;
    }

    const std::set<std::string> expected{"finding", "log", "output", "warn", "write"};
    EXPECT_EQ(kinds, expected);
}
