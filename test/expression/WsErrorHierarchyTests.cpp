// Tests for the cajeta.io.net.ws WebSocket error hierarchy — plan item
// NET-10.8 ("WS error hierarchy: HandshakeRejected, ProtocolViolation,
// MessageTooLarge, ConnectionClosed (with close code)").
//
// The WS exception family is pure cajeta logic (no sockets): a set of
// leaves under the intermediate WebSocketException root, which itself
// descends from cajeta.io.net.NetException. So — exactly like
// NetExceptionTests / UriParseTests — these are golden-vector gtests over
// the JIT: compile a tiny cajeta `run()` that throws/inspects a WS
// exception and returns an int32 sentinel, then assert the value.
//
// What NET-10.8 adds over the two leaves the frame codec (NET-10.3)
// already shipped (WebSocketException root + ProtocolViolationException):
// the ConnectionClosedException leaf carrying an RFC 6455 §7.4 close code
// + optional reason + a clean/abnormal flag, plus the WsCloseCode close-
// code constants holder it discriminates on. HandshakeRejectedException
// and MessageTooLargeException already landed with the handshake /
// fragmentation layers (NET-10.2 / NET-10.4) but are pinned here as the
// family's full taxonomy gate. Mirrors NetExceptionTests' catch-by-subtype
// and catch-as-root assertions.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.io.net.NetException;\n"
        "import cajeta.io.net.ws.WebSocketException;\n"
        "import cajeta.io.net.ws.ProtocolViolationException;\n"
        "import cajeta.io.net.ws.HandshakeRejectedException;\n"
        "import cajeta.io.net.ws.MessageTooLargeException;\n"
        "import cajeta.io.net.ws.ConnectionClosedException;\n"
        "import cajeta.io.net.ws.WsCloseCode;\n"
        "public final class W {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.W");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// --- Every WS leaf is catchable as the WebSocketException family root -----
// The whole point of the intermediate root: one `catch (WebSocketException)`
// nets every WS protocol fault. Each leaf, thrown, is caught here and its
// inherited `kind` read back to prove the dynamic type carried its tag.

TEST(WsErrorHierarchyTests, protocolViolationCaughtAsWsRoot) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap ProtocolViolationException(\"reserved opcode\", (int64) 7);\n"
        "} catch (WebSocketException e) {\n"
        "    return e.kind;\n"   // KIND_INVALID → 12
        "}"), 12);
}

TEST(WsErrorHierarchyTests, handshakeRejectedCaughtAsWsRoot) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap HandshakeRejectedException(\"no upgrade header\");\n"
        "} catch (WebSocketException e) {\n"
        "    return e.kind;\n"   // KIND_INVALID → 12
        "}"), 12);
}

TEST(WsErrorHierarchyTests, messageTooLargeCaughtAsWsRoot) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap MessageTooLargeException(\"over cap\", (int64) 1048576);\n"
        "} catch (WebSocketException e) {\n"
        "    return e.kind;\n"   // KIND_INVALID → 12
        "}"), 12);
}

TEST(WsErrorHierarchyTests, connectionClosedCaughtAsWsRoot) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap ConnectionClosedException(\"peer closed\", 1000);\n"
        "} catch (WebSocketException e) {\n"
        "    return e.kind;\n"   // KIND_OTHER → 99 (a stream end, not invalid input)
        "}"), 99);
}

// --- And as the NetException universal root -------------------------------
// Every WS fault still descends from NetException, so a request-boundary
// `catch (NetException)` nets WS faults alongside transport ones.

TEST(WsErrorHierarchyTests, connectionClosedCaughtAsNetRoot) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap ConnectionClosedException(\"peer closed\", 1001);\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"   // → 99
        "}"), 99);
}

TEST(WsErrorHierarchyTests, protocolViolationCaughtAsNetRoot) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap ProtocolViolationException(\"client frame is not masked\");\n"
        "} catch (NetException e) {\n"
        "    return e.kind;\n"   // → 12
        "}"), 12);
}

// --- The mapped exception is the right *runtime* type (typed catch) -------
// Beyond the kind tag: throwing a leaf and catching it by its own specific
// subtype proves the dynamic type is correct, the more-specific clause
// winning over the broader WebSocketException one.

TEST(WsErrorHierarchyTests, connectionClosedCaughtBySpecificSubtype) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap ConnectionClosedException(\"peer closed\", 1002);\n"
        "} catch (ConnectionClosedException e) {\n"
        "    return e.closeCode;\n"   // proves it is the closed subtype → 1002
        "} catch (WebSocketException other) {\n"
        "    return 0;\n"
        "}"), 1002);
}

TEST(WsErrorHierarchyTests, messageTooLargeCaughtBySpecificSubtype) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    throw heap MessageTooLargeException(\"over cap\", (int64) 65536);\n"
        "} catch (MessageTooLargeException e) {\n"
        "    return (int32) e.limit;\n"   // → 65536
        "} catch (WebSocketException other) {\n"
        "    return 0;\n"
        "}"), 65536);
}

// --- ConnectionClosed carries the close code + reason + clean flag --------
// The NET-10.8 deliverable: "ConnectionClosed (with close code)". Pins each
// of the three constructor forms.

TEST(WsErrorHierarchyTests, closedCleanWithCodeReportsCode) {
    EXPECT_EQ(runI32(
        "ConnectionClosedException e = heap ConnectionClosedException(\"bye\", 1000);\n"
        "return (e.closeCode == 1000 && e.isClean) ? 1 : 0;"), 1);
}

TEST(WsErrorHierarchyTests, closedAbnormalReportsAbnormalCodeAndNotClean) {
    // The (message)-only ctor is the abnormal close: code 1006, not clean.
    EXPECT_EQ(runI32(
        "ConnectionClosedException e = heap ConnectionClosedException(\"transport dropped\");\n"
        "return (e.closeCode == WsCloseCode.ABNORMAL && !e.isClean) ? 1 : 0;"), 1);
}

TEST(WsErrorHierarchyTests, closedAbnormalCodeIs1006) {
    EXPECT_EQ(runI32(
        "ConnectionClosedException e = heap ConnectionClosedException(\"transport dropped\");\n"
        "return e.closeCode;"), 1006);
}

TEST(WsErrorHierarchyTests, closedWithReasonCarriesReason) {
    EXPECT_EQ(runI32(
        "ConnectionClosedException e =\n"
        "    heap ConnectionClosedException(\"bye\", 1000, \"shutting down\");\n"
        "return (e.reason != null && e.reason.equals(\"shutting down\")) ? 1 : 0;"), 1);
}

TEST(WsErrorHierarchyTests, closedCodeOnlyHasNullReason) {
    // The (message, code) ctor leaves reason null (0) — a code-only close.
    EXPECT_EQ(runI32(
        "ConnectionClosedException e = heap ConnectionClosedException(\"bye\", 1000);\n"
        "return (e.reason == null) ? 1 : 0;"), 1);
}

// --- HandshakeRejected carries the HTTP status it maps to -----------------

TEST(WsErrorHierarchyTests, handshakeRejectedDefaultStatusIs400) {
    EXPECT_EQ(runI32(
        "HandshakeRejectedException e = heap HandshakeRejectedException(\"bad request\");\n"
        "return e.httpStatus;"), 400);
}

TEST(WsErrorHierarchyTests, handshakeRejectedExplicitStatusPassesThrough) {
    EXPECT_EQ(runI32(
        "HandshakeRejectedException e =\n"
        "    heap HandshakeRejectedException(\"version mismatch\", 426);\n"
        "return e.httpStatus;"), 426);
}

// --- WsCloseCode constants + classifiers ----------------------------------
// The close-code holder NET-10.8 introduces for ConnectionClosed to carry.

TEST(WsErrorHierarchyTests, closeCodeNamedConstants) {
    // NORMAL 1000, PROTOCOL_ERROR 1002, MESSAGE_TOO_BIG 1009 — the codes
    // the WS leaves map to (RFC 6455 §7.4.1).
    EXPECT_EQ(runI32(
        "return (WsCloseCode.NORMAL == 1000\n"
        "     && WsCloseCode.PROTOCOL_ERROR == 1002\n"
        "     && WsCloseCode.MESSAGE_TOO_BIG == 1009) ? 1 : 0;"), 1);
}

TEST(WsErrorHierarchyTests, closeCodeSendableExcludesReservedCodes) {
    // 1005/1006/1015 are reserved (never on the wire); 1000 + a private-use
    // 4000 code are sendable.
    EXPECT_EQ(runI32(
        "boolean ok = WsCloseCode.isSendable(1000)\n"
        "          && WsCloseCode.isSendable(4000)\n"
        "          && !WsCloseCode.isSendable(WsCloseCode.NO_STATUS)\n"
        "          && !WsCloseCode.isSendable(WsCloseCode.ABNORMAL)\n"
        "          && !WsCloseCode.isSendable(WsCloseCode.TLS_HANDSHAKE)\n"
        "          && !WsCloseCode.isSendable(999);\n"
        "return ok ? 1 : 0;"), 1);
}

TEST(WsErrorHierarchyTests, closeCodePrivateUseRange) {
    EXPECT_EQ(runI32(
        "return (WsCloseCode.isPrivateUse(4000)\n"
        "     && WsCloseCode.isPrivateUse(4999)\n"
        "     && !WsCloseCode.isPrivateUse(3999)\n"
        "     && !WsCloseCode.isPrivateUse(5000)) ? 1 : 0;"), 1);
}
