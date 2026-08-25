// plugin-output-protocol plan §0 — the conformance suite's foundation.
//
// "The plugin conforms to spec" has to be executable, or every later section
// accepts itself on its own terms. These pin the ONE definition of a valid
// record — the same code `PluginRuntime` dispatches on, so conformance and
// runtime behaviour cannot disagree.
//
// The motivating defect: `dev.cajeta.coverage` builds its records by string
// concatenation with no escaping, so a `"` or a newline in a message emits
// malformed JSON. The build tool then failed the parse or silently mis-read
// the record, and the plugin appeared broken for a reason nothing reported.

#include <gtest/gtest.h>

#include "cajeta/buildtool/PluginRecord.h"

using namespace cajeta::buildtool;

namespace {

    llvm::json::Object obj(std::initializer_list<
                           std::pair<llvm::StringRef, llvm::json::Value>> kv) {
        llvm::json::Object o;
        for (auto& p : kv) o[p.first] = p.second;
        return o;
    }

} // namespace

// ---- 0.1.1 — a missing required field is caught -----------------------------
//
// The suite must FAIL a plugin that omits a required field. A conformance
// suite that only ever passes proves nothing.

TEST(PluginRecordTests, aKnownKindMissingItsRequiredFieldIsMalformed) {
    auto check = checkPluginRecord(obj({{"kind", "log"}}));
    EXPECT_EQ(RecordVerdict::Malformed, check.verdict);
    EXPECT_NE(std::string::npos, check.reason.find("message"))
        << "the reason must name the missing field, or the author cannot act "
           "on it: " << check.reason;
}

TEST(PluginRecordTests, everyKindsRequiredFieldsAreEnforced) {
    struct Case { llvm::json::Object record; const char* missing; };
    std::vector<Case> cases;
    cases.push_back({obj({{"kind", "warn"}}), "message"});
    cases.push_back({obj({{"kind", "write"}}), "text"});
    cases.push_back({obj({{"kind", "output"}, {"key", "k"}}), "value"});
    cases.push_back({obj({{"kind", "output"}, {"value", "v"}}), "key"});
    cases.push_back({obj({{"kind", "finding"}, {"message", "m"}}), "severity"});
    cases.push_back({obj({{"kind", "finding"}, {"severity", "error"}}), "message"});
    cases.push_back({obj({{"kind", "result"}}), "status"});
    cases.push_back({obj({{"kind", "error"}}), "message"});

    for (auto& c : cases) {
        auto check = checkPluginRecord(c.record);
        EXPECT_EQ(RecordVerdict::Malformed, check.verdict)
            << "expected a record missing '" << c.missing << "' to be malformed";
        EXPECT_NE(std::string::npos, check.reason.find(c.missing)) << check.reason;
    }
}

TEST(PluginRecordTests, aRecordWithNoKindIsMalformed) {
    auto check = checkPluginRecord(obj({{"message", "orphan"}}));
    EXPECT_EQ(RecordVerdict::Malformed, check.verdict);
}

TEST(PluginRecordTests, aFieldOfTheWrongTypeIsMalformed) {
    auto check = checkPluginRecord(obj({{"kind", "log"}, {"message", 7}}));
    EXPECT_EQ(RecordVerdict::Malformed, check.verdict)
        << "a numeric 'message' is not a message";
}

// ---- the other half: valid records must PASS --------------------------------
//
// Without these, a validator that rejected everything would satisfy the checks
// above. Both directions, as the plan requires.

TEST(PluginRecordTests, wellFormedRecordsOfEveryKindAreValid) {
    std::vector<llvm::json::Object> good;
    good.push_back(obj({{"kind", "log"}, {"message", "progress"}}));
    good.push_back(obj({{"kind", "log"}, {"message", "m"}, {"level", "warn"}}));
    good.push_back(obj({{"kind", "warn"}, {"message", "careful"}}));
    good.push_back(obj({{"kind", "write"}, {"text", "raw"}}));
    good.push_back(obj({{"kind", "output"}, {"key", "k"}, {"value", "v"}}));
    good.push_back(obj({{"kind", "finding"},
                        {"severity", "warning"}, {"message", "m"}}));
    good.push_back(obj({{"kind", "result"}, {"status", "ok"}}));

    for (auto& g : good) {
        auto check = checkPluginRecord(g);
        EXPECT_TRUE(check.ok()) << "rejected a valid record: " << check.reason;
    }
}

TEST(PluginRecordTests, optionalFindingFieldsAreOptional) {
    // spec §5 use case 3: an unlocated finding must produce a diagnostic with
    // NO location, never a fabricated one — so absence has to be legal.
    auto bare = checkPluginRecord(
        obj({{"kind", "finding"}, {"severity", "note"}, {"message", "m"}}));
    EXPECT_TRUE(bare.ok()) << bare.reason;

    auto located = checkPluginRecord(obj({{"kind", "finding"},
                                          {"severity", "error"},
                                          {"message", "m"},
                                          {"file", "A.cajeta"},
                                          {"line", 12},
                                          {"column", 3},
                                          {"rule", "coverage/min"}}));
    EXPECT_TRUE(located.ok()) << located.reason;
}

// ---- an unknown kind is not malformed ---------------------------------------

TEST(PluginRecordTests, anUnknownKindIsUnrecognisedRatherThanMalformed) {
    auto check = checkPluginRecord(obj({{"kind", "telemetry"}}));
    EXPECT_EQ(RecordVerdict::UnknownKind, check.verdict)
        << "a newer plugin emitting a kind this build has never heard of is "
           "not malformed; refusing it would make every build tool a ceiling "
           "on every plugin";
    EXPECT_NE(std::string::npos, check.reason.find("telemetry")) << check.reason;
}

// ---- 0.1.2 / §4.1 — echoing untrusted input safely --------------------------
//
// The offending line is bytes the plugin controls and is malformed by
// definition. Reporting it must not damage the stream reporting it — that
// would be this spec's own failure mode, one layer up.

TEST(PluginRecordTests, quotingEscapesWhatWouldBreakTheStream) {
    EXPECT_EQ("a\\\"b", quoteUntrustedLine("a\"b"));
    EXPECT_EQ("a\\\\b", quoteUntrustedLine("a\\b"));
}

TEST(PluginRecordTests, quotingCannotForgeASecondLineOrRecord) {
    auto q = quoteUntrustedLine("first\n{\"kind\":\"result\",\"status\":\"ok\"}");
    EXPECT_EQ(std::string::npos, q.find('\n'))
        << "a raw newline survived quoting, so a malformed record could forge "
           "a second console line or a second record: " << q;
    EXPECT_NE(std::string::npos, q.find("\\n"));
}

TEST(PluginRecordTests, quotingEscapesControlCharacters) {
    auto q = quoteUntrustedLine(llvm::StringRef("a\x01\x1b" "b", 4));
    EXPECT_EQ(std::string::npos, q.find('\x01'));
    EXPECT_EQ(std::string::npos, q.find('\x1b'))
        << "an escape character survived, which can rewrite a terminal: " << q;
}

TEST(PluginRecordTests, quotingBoundsAnOverlongLine) {
    std::string huge(64 * 1024, 'x');
    auto q = quoteUntrustedLine(huge);
    EXPECT_LT(q.size(), size_t(400))
        << "a plugin emitting a megabyte of garbage must not flood the log";
    EXPECT_NE(std::string::npos, q.find("truncated"))
        << "truncation must be visible, not silent: " << q;
}

TEST(PluginRecordTests, quotingSurvivesInvalidUtf8) {
    // Lone continuation bytes — not valid UTF-8 in any position.
    auto q = quoteUntrustedLine(llvm::StringRef("ok\x80\xff", 4));
    for (char c : q) {
        EXPECT_LT(static_cast<unsigned char>(c), 0x80u)
            << "invalid UTF-8 was reproduced rather than escaped, so the "
               "output encoding is no longer trustworthy: " << q;
    }
}

TEST(PluginRecordTests, quotingLeavesOrdinaryTextAlone) {
    // The other direction: a validator that escaped everything would pass the
    // checks above and make every message unreadable.
    EXPECT_EQ("coco: [3/6] instrumenting 6 of 10 modules",
              quoteUntrustedLine("coco: [3/6] instrumenting 6 of 10 modules"));
}

// =============================================================================
// plan §0.1 — the conformance suite fails what it must
//
// A suite that only ever passes proves nothing, so each of these asserts a
// REJECTION, with the accepting case beside it. The three rejected shapes are
// the three the plan names: a missing required field, unescaped output, and an
// action that never reports how it finished.
// =============================================================================

namespace {

    // What a plugin emitted, as the conformance suite sees it.
    std::vector<std::string> stream(std::initializer_list<const char*> lines) {
        return std::vector<std::string>(lines.begin(), lines.end());
    }

    bool mentions(const ConformanceReport& r, llvm::StringRef needle) {
        for (const auto& p : r.problems) {
            if (llvm::StringRef(p).contains(needle)) return true;
        }
        return false;
    }

} // namespace

TEST(PluginConformanceTests, passesAPluginThatEmitsWellFormedRecords) {
    auto r = checkPluginStream(stream({
        R"({"kind":"log","message":"coco: [1/6] reference pass"})",
        R"({"kind":"finding","severity":"warning","message":"survivor","file":"Shipping.cajeta","line":25})",
        R"({"kind":"output","key":"percent","value":"36.0"})",
        R"({"kind":"result","status":"ok"})",
    }));
    EXPECT_TRUE(r.passed) << (r.problems.empty() ? "" : r.problems.front());
}

// ---- 0.1.1 — a missing required field --------------------------------------

TEST(PluginConformanceTests, failsAPluginOmittingARequiredField) {
    auto r = checkPluginStream(stream({
        R"({"kind":"finding","severity":"error"})",   // no message
        R"({"kind":"result","status":"ok"})",
    }));
    EXPECT_FALSE(r.passed);
    EXPECT_TRUE(mentions(r, "message"))
        << "the report must name the missing field so the author can act on it";
}

// ---- 0.1.2 — unescaped output ----------------------------------------------
//
// The defect that motivated the whole spec. coco concatenates its JSON, so a
// message containing a quote emits a record that is not JSON at all.

TEST(PluginConformanceTests, failsAPluginThatDidNotEscapeItsMessage) {
    // Exactly what coco's hand-built emitter produces for a message
    // containing a quote: the value terminates early and the rest is garbage.
    auto r = checkPluginStream(stream({
        R"({"kind":"log","level":"info","message":"unknown type "Runner""})",
        R"({"kind":"result","status":"ok"})",
    }));
    EXPECT_FALSE(r.passed);
    EXPECT_TRUE(mentions(r, "not valid JSON")) << "unescaped output must fail";
}

TEST(PluginConformanceTests, theFailureReportQuotesTheOffendingLineSafely) {
    auto r = checkPluginStream(stream({
        "garbage\twith\ta tab",
        R"({"kind":"result","status":"ok"})",
    }));
    ASSERT_FALSE(r.passed);
    for (const auto& p : r.problems) {
        EXPECT_EQ(std::string::npos, p.find('\t'))
            << "the offending line was reproduced raw into the report: " << p;
    }
}

// ---- 0.1.3 — never reports how it finished ----------------------------------

TEST(PluginConformanceTests, failsAPluginThatNeverEmitsAResult) {
    auto r = checkPluginStream(stream({
        R"({"kind":"log","message":"working"})",
    }));
    EXPECT_FALSE(r.passed);
    EXPECT_TRUE(mentions(r, "result"));
}

TEST(PluginConformanceTests, failsAPluginThatReportsTwice) {
    auto r = checkPluginStream(stream({
        R"({"kind":"result","status":"ok"})",
        R"({"kind":"result","status":"error","message":"or was it"})",
    }));
    EXPECT_FALSE(r.passed)
        << "two results leave the build tool to choose which one was true";
}

// ---- forward compatibility is not non-conformance ---------------------------

TEST(PluginConformanceTests, anUnknownKindDoesNotFailConformance) {
    auto r = checkPluginStream(stream({
        R"({"kind":"telemetry","spans":3})",
        R"({"kind":"result","status":"ok"})",
    }));
    EXPECT_TRUE(r.passed)
        << "a plugin emitting a kind this build has never heard of is ahead of "
           "it, not wrong; refusing would make every build tool a ceiling on "
           "every plugin";
}

// ---- conformance is stricter than the runtime's tolerance -------------------

TEST(PluginConformanceTests, rawTextFailsConformanceThoughTheRuntimeTolerates) {
    // §4 use case 4: the RUNTIME wraps raw stdout as a log so printf debugging
    // keeps working. A plugin that SHIPS raw text has still not conformed.
    // Lenient at runtime, strict here — that asymmetry is what lets the
    // protocol hold without breaking anyone mid-build.
    auto r = checkPluginStream(stream({
        "just some printf output",
        R"({"kind":"result","status":"ok"})",
    }));
    EXPECT_FALSE(r.passed);
}

TEST(PluginConformanceTests, blankLinesAreIgnored) {
    auto r = checkPluginStream(stream({
        "",
        R"({"kind":"result","status":"ok"})",
        "   ",
    }));
    EXPECT_TRUE(r.passed) << "trailing newlines are not a protocol violation";
}
