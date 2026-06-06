// NET-7.6 — cajeta.net.http.KeepAlive tests (keep-alive semantics).
//
// Keep-alive is pure policy logic over the Headers map + the HTTP
// version defaults (RFC 7230 §6.3) — no I/O — so it tests directly over
// the JIT, exactly like HttpParserTests / HttpSerializerTests: each test
// compiles a small Cajeta `run()` that builds headers, asks the KeepAlive
// policy a question, and returns an int32 sentinel (positive on success,
// a distinct negative per failed sub-check).
//
// Pins the NET-7.6 deliverable: "Keep-alive semantics: parse/emit
// Connection: keep-alive/close, decide reuse vs close per HTTP/1.1
// defaults (1.1 = keep-alive unless close)." (plan/cajeta-net-plan.md,
// Phase 7) and its acceptance row reuseDecisionVectors.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Wrap a method body in a class importing the keep-alive policy, the
// header map, and String. The body must `return` int32.
int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.Headers;\n"
        "import cajeta.net.http.KeepAlive;\n"
        "public final class M {\n"
        "    // Build a one-entry Connection header map (or an empty map\n"
        "    // when value is empty).\n"
        "    static #Headers conn(String value) {\n"
        "        Headers h = heap Headers();\n"
        "        if (value.byteLength > 0) { h.add(\"Connection\", value); }\n"
        "        return #h;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- per-message reuse read: the version-default vector table ----------

// THE acceptance row (reuseDecisionVectors): the per-message reuse read
// matches the RFC 7230 §6.3 HTTP/1.0 and 1.1 defaults across the full
// (version × Connection-token) matrix.
TEST(HttpKeepAliveTests, reuseDecisionVectors) {
    EXPECT_EQ(runI32(
        // 1.1, no Connection header -> persistent (default keep-alive).
        "if (!KeepAlive.messageAllowsReuse(\"HTTP/1.1\", M.conn(\"\"))) return -1;\n"
        // 1.1, explicit close -> not reusable.
        "if (KeepAlive.messageAllowsReuse(\"HTTP/1.1\", M.conn(\"close\"))) return -2;\n"
        // 1.1, explicit keep-alive -> reusable (redundant but honored).
        "if (!KeepAlive.messageAllowsReuse(\"HTTP/1.1\", M.conn(\"keep-alive\"))) return -3;\n"
        // 1.0, no Connection header -> NOT persistent (default close).
        "if (KeepAlive.messageAllowsReuse(\"HTTP/1.0\", M.conn(\"\"))) return -4;\n"
        // 1.0, explicit keep-alive -> reusable (must opt in).
        "if (!KeepAlive.messageAllowsReuse(\"HTTP/1.0\", M.conn(\"keep-alive\"))) return -5;\n"
        // 1.0, explicit close -> not reusable.
        "if (KeepAlive.messageAllowsReuse(\"HTTP/1.0\", M.conn(\"close\"))) return -6;\n"
        "return 1;"), 1);
}

// The Connection header value is a case-insensitive comma list: a token
// is matched whole (delimited), not as a substring, and case folds.
TEST(HttpKeepAliveTests, connectionTokenIsCaseInsensitiveAndDelimited) {
    EXPECT_EQ(runI32(
        // Mixed case + a second option (Upgrade) -> still keep-alive.
        "if (!KeepAlive.messageAllowsReuse(\"HTTP/1.1\", M.conn(\"Keep-Alive, Upgrade\"))) return -1;\n"
        // Mixed-case close.
        "if (KeepAlive.messageAllowsReuse(\"HTTP/1.1\", M.conn(\"Close\"))) return -2;\n"
        // A value that merely *contains* the letters of "close" inside a
        // larger token must NOT match (the substring guard).
        "if (!KeepAlive.messageAllowsReuse(\"HTTP/1.1\", M.conn(\"closely\"))) return -3;\n"
        "return 1;"), 1);
}

// close wins over keep-alive when a single header carries both (a sender
// error, but close is the conservative read).
TEST(HttpKeepAliveTests, closeWinsOverKeepAlive) {
    EXPECT_EQ(runI32(
        "if (KeepAlive.messageAllowsReuse(\"HTTP/1.1\", M.conn(\"keep-alive, close\"))) return -1;\n"
        "if (KeepAlive.messageAllowsReuse(\"HTTP/1.1\", M.conn(\"close, keep-alive\"))) return -2;\n"
        "return 1;"), 1);
}

// --- the exchange-level decision ---------------------------------------

// canReuse is the conjunction: reusable iff BOTH request and response
// permit it. Either side's close ends reuse.
TEST(HttpKeepAliveTests, canReuseRequiresBothSides) {
    EXPECT_EQ(runI32(
        // both 1.1 default -> reuse.
        "if (!KeepAlive.canReuse(\"HTTP/1.1\", M.conn(\"\"), \"HTTP/1.1\", M.conn(\"\"))) return -1;\n"
        // response closes -> no reuse.
        "if (KeepAlive.canReuse(\"HTTP/1.1\", M.conn(\"\"), \"HTTP/1.1\", M.conn(\"close\"))) return -2;\n"
        // request closes -> no reuse.
        "if (KeepAlive.canReuse(\"HTTP/1.1\", M.conn(\"close\"), \"HTTP/1.1\", M.conn(\"\"))) return -3;\n"
        // request is 1.0 default (close) even though response 1.1 -> no reuse.
        "if (KeepAlive.canReuse(\"HTTP/1.0\", M.conn(\"\"), \"HTTP/1.1\", M.conn(\"\"))) return -4;\n"
        // 1.0 both sides opt in -> reuse.
        "if (!KeepAlive.canReuse(\"HTTP/1.0\", M.conn(\"keep-alive\"), \"HTTP/1.0\", M.conn(\"keep-alive\"))) return -5;\n"
        "return 1;"), 1);
}

// responseAllowsReuse is the client convenience: the response-only read.
TEST(HttpKeepAliveTests, responseAllowsReuseReadsResponseOnly) {
    EXPECT_EQ(runI32(
        "if (!KeepAlive.responseAllowsReuse(\"HTTP/1.1\", M.conn(\"\"))) return -1;\n"
        "if (KeepAlive.responseAllowsReuse(\"HTTP/1.1\", M.conn(\"close\"))) return -2;\n"
        "if (KeepAlive.responseAllowsReuse(\"HTTP/1.0\", M.conn(\"\"))) return -3;\n"
        "return 1;"), 1);
}

// --- emit: the canonical Connection token ------------------------------

// connectionToken emits only when the version default doesn't already
// convey the intent: minimal-wire policy (RFC 7230 §6.3).
TEST(HttpKeepAliveTests, connectionTokenMinimalWire) {
    EXPECT_EQ(runI32(
        // 1.1 + want reuse -> null (1.1 is persistent by default).
        "if (KeepAlive.connectionToken(\"HTTP/1.1\", true) != null) return -1;\n"
        // 1.1 + want close -> \"close\" (must opt out).
        "String c = KeepAlive.connectionToken(\"HTTP/1.1\", false);\n"
        "if (c == null) return -2;\n"
        "if (!c.equals(\"close\")) return -3;\n"
        // 1.0 + want reuse -> \"keep-alive\" (must opt in).
        "String k = KeepAlive.connectionToken(\"HTTP/1.0\", true);\n"
        "if (k == null) return -4;\n"
        "if (!k.equals(\"keep-alive\")) return -5;\n"
        // 1.0 + want close -> null (close is the 1.0 default).
        "if (KeepAlive.connectionToken(\"HTTP/1.0\", false) != null) return -6;\n"
        "return 1;"), 1);
}

// connectionTokenExplicit always states the intent, never null.
TEST(HttpKeepAliveTests, connectionTokenExplicitAlwaysStated) {
    EXPECT_EQ(runI32(
        "if (!KeepAlive.connectionTokenExplicit(true).equals(\"keep-alive\")) return -1;\n"
        "if (!KeepAlive.connectionTokenExplicit(false).equals(\"close\")) return -2;\n"
        "return 1;"), 1);
}

// applyConnectionHeader sets the header when needed and removes it when
// the version default conveys the intent (minimal wire).
TEST(HttpKeepAliveTests, applyConnectionHeaderSetsOrRemoves) {
    EXPECT_EQ(runI32(
        // 1.1 + want close -> sets Connection: close.
        "Headers h1 = heap Headers();\n"
        "KeepAlive.applyConnectionHeader(h1, \"HTTP/1.1\", false);\n"
        "if (!h1.contains(\"connection\")) return -1;\n"
        "if (!h1.get(\"connection\").equals(\"close\")) return -2;\n"
        // 1.1 + want reuse -> removes any Connection (default conveys it).
        "Headers h2 = heap Headers();\n"
        "h2.add(\"Connection\", \"close\");\n"               // pre-existing close
        "KeepAlive.applyConnectionHeader(h2, \"HTTP/1.1\", true);\n"
        "if (h2.contains(\"connection\")) return -3;\n"
        // 1.0 + want reuse -> sets Connection: keep-alive.
        "Headers h3 = heap Headers();\n"
        "KeepAlive.applyConnectionHeader(h3, \"HTTP/1.0\", true);\n"
        "if (!h3.get(\"connection\").equals(\"keep-alive\")) return -4;\n"
        "return 1;"), 1);
}

// An unrecognized version is treated as HTTP/1.1 (persistent) — the
// modern safe default.
TEST(HttpKeepAliveTests, unknownVersionDefaultsPersistent) {
    EXPECT_EQ(runI32(
        "if (!KeepAlive.messageAllowsReuse(\"HTTP/2\", M.conn(\"\"))) return -1;\n"
        "if (KeepAlive.connectionToken(\"HTTP/2\", true) != null) return -2;\n"
        "return 1;"), 1);
}
