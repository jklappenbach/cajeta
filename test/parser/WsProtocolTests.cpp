// NET-10.6 — cajeta.net.ws WebSocket read-side protocol engine (WsProtocol).
//
// The WebSocket facade (WebSocket.cajeta) does live socket I/O over an
// AsyncReader/AsyncWriter with a fiber-aware write Lock — not golden-vector
// testable at this layer. Its *decision* core, though — what the read loop
// must do with each decoded frame (deliver a message, auto-pong a ping,
// complete the close handshake, swallow a pong) — is pure logic in
// WsProtocol -> WsReadAction, so it tests directly over the JIT exactly the
// way WsMessageAssemblerTests (NET-10.4) and WsControlFrameTests (NET-10.5)
// drive their pure layers: compile a small Cajeta `run()` that feeds frames
// to a WsProtocol and returns an int32 sentinel (positive on success, a
// distinct negative per failed sub-check).
//
// Pins the NET-10.6 deliverable: "`WebSocket` API: send/receive/close.
// Concurrent read + write from separate fibers safely (a write mutex)."
// (plan/cajeta-net-plan.md, Phase 10) — specifically the read-loop dispatch
// the concurrent reader fiber runs, and the acceptance rows
// fragmentationWithInterleavedPing (the auto-pong-mid-fragment half) and
// closeHandshakeReportsCode (the close-handshake half).

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.ws.WsOpcode;\n"
        "import cajeta.net.ws.WsFrame;\n"
        "import cajeta.net.ws.WsMessage;\n"
        "import cajeta.net.ws.WsCloseCode;\n"
        "import cajeta.net.ws.WsCloseReason;\n"
        "import cajeta.net.ws.WsControlFrames;\n"
        "import cajeta.net.ws.WsReadAction;\n"
        "import cajeta.net.ws.WsProtocol;\n"
        "import cajeta.net.ws.ProtocolViolationException;\n"
        "import cajeta.net.ws.MessageTooLargeException;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Emit Cajeta that declares `int8[] <name> = new int8[N];` filled from an
// ASCII string literal's bytes (single-byte chars only).
std::string emitAscii(const std::string& name, const std::string& text) {
    std::string s = "int8[] " + name + " = new int8[" +
                    std::to_string(text.size()) + "];\n";
    for (size_t i = 0; i < text.size(); i++) {
        s += name + "[" + std::to_string(i) + "] = (int8) " +
             std::to_string((int)(unsigned char)text[i]) + ";\n";
    }
    return s;
}

} // namespace

// --- a single unfragmented data frame delivers a MESSAGE ----------------
TEST(WsProtocolTests, singleFrameYieldsMessageAction) {
    EXPECT_EQ(runI32(
        emitAscii("p", "Hello") +
        "WsFrame f = WsFrame.of(true, WsOpcode.TEXT, false, #p);\n"
        "WsProtocol proto = WsProtocol.create();\n"
        "WsReadAction a = proto.onFrame(f);\n"
        "if (a.getKind() != WsReadAction.MESSAGE) return -1;\n"
        "if (!a.isMessage()) return -2;\n"
        "WsMessage m = a.getMessage();\n"
        "if (!m.isText()) return -3;\n"
        "if (m.length() != 5) return -4;\n"
        "if (m.getPayload()[0] != (int8) 72) return -5;\n"   // 'H'
        "if (m.getPayload()[4] != (int8) 111) return -6;\n"  // 'o'
        "return 1;"), 1);
}

// --- a fragmented message: NONE on each non-final fragment, MESSAGE last -
TEST(WsProtocolTests, fragmentsReassembleAcrossActions) {
    EXPECT_EQ(runI32(
        emitAscii("a", "Hel") +
        emitAscii("b", "lo ") +
        emitAscii("c", "Wld") +
        "WsProtocol proto = WsProtocol.create();\n"
        // first fragment: FIN=0 text
        "WsFrame f0 = WsFrame.of(false, WsOpcode.TEXT, false, #a);\n"
        "WsReadAction a0 = proto.onFrame(f0);\n"
        "if (a0.getKind() != WsReadAction.NONE) return -1;\n"
        // continuation, FIN=0
        "WsFrame f1 = WsFrame.of(false, WsOpcode.CONTINUATION, false, #b);\n"
        "WsReadAction a1 = proto.onFrame(f1);\n"
        "if (a1.getKind() != WsReadAction.NONE) return -2;\n"
        // final continuation, FIN=1
        "WsFrame f2 = WsFrame.of(true, WsOpcode.CONTINUATION, false, #c);\n"
        "WsReadAction a2 = proto.onFrame(f2);\n"
        "if (a2.getKind() != WsReadAction.MESSAGE) return -3;\n"
        "WsMessage m = a2.getMessage();\n"
        "if (m.length() != 9) return -4;\n"           // "Hello Wld"
        "if (m.getPayload()[0] != (int8) 72) return -5;\n"   // 'H'
        "if (m.getPayload()[8] != (int8) 100) return -6;\n"  // 'd'
        "return 1;"), 1);
}

// --- auto-pong: an inbound ping yields a SEND_FRAME pong echoing payload -
//
// The fragmentationWithInterleavedPing acceptance row, engine half: a ping
// arriving mid-fragment is answered (auto-pong) WITHOUT disturbing the
// in-progress message.
TEST(WsProtocolTests, pingAutoPongsAndPreservesFragment) {
    EXPECT_EQ(runI32(
        emitAscii("a", "Hel") +
        emitAscii("c", "lo") +
        "int8[] pingPay = new int8[2];\n"
        "pingPay[0] = (int8) 1;\n"
        "pingPay[1] = (int8) 2;\n"
        "WsProtocol proto = WsProtocol.create();\n"
        // open a fragmented message
        "WsFrame f0 = WsFrame.of(false, WsOpcode.TEXT, false, #a);\n"
        "if (proto.onFrame(f0).getKind() != WsReadAction.NONE) return -1;\n"
        "if (!proto.isAssembling()) return -2;\n"
        // a ping interleaves: auto-pong, message untouched
        "WsFrame ping = WsControlFrames.ping(#pingPay);\n"
        "WsReadAction pa = proto.onFrame(ping);\n"
        "if (pa.getKind() != WsReadAction.SEND_FRAME) return -3;\n"
        "WsFrame pong = pa.getFrame();\n"
        "if (pong.getOpcode() != WsOpcode.PONG) return -4;\n"
        "if (pong.payloadLength() != 2) return -5;\n"
        "if (pong.getPayload()[0] != (int8) 1) return -6;\n"
        "if (!proto.isAssembling()) return -7;\n"   // fragment preserved
        // finish the message
        "WsFrame f1 = WsFrame.of(true, WsOpcode.CONTINUATION, false, #c);\n"
        "WsReadAction fa = proto.onFrame(f1);\n"
        "if (fa.getKind() != WsReadAction.MESSAGE) return -8;\n"
        "if (fa.getMessage().length() != 5) return -9;\n"   // "Hello"
        "return 1;"), 1);
}

// --- auto-pong disabled: an inbound ping surfaces as a PING action -------
TEST(WsProtocolTests, autoPongOffSurfacesPing) {
    EXPECT_EQ(runI32(
        "int8[] pp = new int8[1];\n"
        "pp[0] = (int8) 9;\n"
        "WsProtocol proto = WsProtocol.create();\n"
        "proto.withAutoPong(false);\n"
        "WsFrame ping = WsControlFrames.ping(#pp);\n"
        "WsReadAction a = proto.onFrame(ping);\n"
        "if (a.getKind() != WsReadAction.PING) return -1;\n"
        "if (a.getFrame().getOpcode() != WsOpcode.PING) return -2;\n"
        "return 1;"), 1);
}

// --- an inbound pong is swallowed (NONE) --------------------------------
TEST(WsProtocolTests, pongIsSwallowed) {
    EXPECT_EQ(runI32(
        "WsFrame pong = WsControlFrames.pong(new int8[0]);\n"
        "WsProtocol proto = WsProtocol.create();\n"
        "WsReadAction a = proto.onFrame(pong);\n"
        "if (a.getKind() != WsReadAction.NONE) return -1;\n"
        "return 1;"), 1);
}

// --- close handshake: peer close yields CLOSED + a reciprocal reply ------
//
// closeHandshakeReportsCode: a received close reports its code, and (the
// local side not having closed) the engine builds the reciprocal close.
TEST(WsProtocolTests, peerCloseYieldsReciprocalAndReportsCode) {
    EXPECT_EQ(runI32(
        "WsFrame peerClose = WsControlFrames.closeCode(WsCloseCode.GOING_AWAY);\n"  // 1001
        "WsProtocol proto = WsProtocol.create();\n"
        "WsReadAction a = proto.onFrame(peerClose);\n"
        "if (a.getKind() != WsReadAction.CLOSED) return -1;\n"
        "if (!a.isClosed()) return -2;\n"
        // the close reason reports the peer code
        "WsCloseReason r = a.getCloseReason();\n"
        "if (!r.hasCode()) return -3;\n"
        "if (r.getCode() != 1001) return -4;\n"
        // a reciprocal close reply is present (local side had not closed)
        "WsFrame reply = a.getFrame();\n"
        "if (reply == null) return -5;\n"
        "if (reply.getOpcode() != WsOpcode.CLOSE) return -6;\n"
        "WsCloseReason rr = WsControlFrames.parseClose(reply);\n"
        "if (rr.getCode() != 1001) return -7;\n"
        // engine is now terminal
        "if (!proto.isClosed()) return -8;\n"
        "return 1;"), 1);
}

// --- close after the local side already closed: no reciprocal reply ------
TEST(WsProtocolTests, peerCloseAfterLocalCloseHasNoReply) {
    EXPECT_EQ(runI32(
        "WsProtocol proto = WsProtocol.create();\n"
        "proto.markLocalCloseSent();\n"
        "WsFrame peerClose = WsControlFrames.closeCode(WsCloseCode.NORMAL);\n"
        "WsReadAction a = proto.onFrame(peerClose);\n"
        "if (a.getKind() != WsReadAction.CLOSED) return -1;\n"
        "if (a.getFrame() != null) return -2;\n"          // no reply
        "WsCloseReason r = a.getCloseReason();\n"
        "if (r.getCode() != 1000) return -3;\n"
        "return 1;"), 1);
}

// --- an empty (code-less) peer close reports NO_STATUS, NORMAL reciprocal-
TEST(WsProtocolTests, emptyCloseReportsNoStatusNormalReply) {
    EXPECT_EQ(runI32(
        "WsFrame peerClose = WsControlFrames.closeEmpty();\n"
        "WsProtocol proto = WsProtocol.create();\n"
        "WsReadAction a = proto.onFrame(peerClose);\n"
        "if (a.getKind() != WsReadAction.CLOSED) return -1;\n"
        "WsCloseReason r = a.getCloseReason();\n"
        "if (r.hasCode()) return -2;\n"                   // no on-wire code
        "if (r.getCode() != WsCloseCode.NO_STATUS) return -3;\n"  // 1005
        // a code-less close gets a NORMAL (1000) reciprocal
        "WsFrame reply = a.getFrame();\n"
        "if (reply == null) return -4;\n"
        "WsCloseReason rr = WsControlFrames.parseClose(reply);\n"
        "if (rr.getCode() != WsCloseCode.NORMAL) return -5;\n"   // 1000
        "return 1;"), 1);
}

// --- after a peer close, every further onFrame stays terminally CLOSED ---
TEST(WsProtocolTests, terminalAfterClose) {
    EXPECT_EQ(runI32(
        emitAscii("p", "ignored") +
        "WsProtocol proto = WsProtocol.create();\n"
        "WsFrame peerClose = WsControlFrames.closeCode(WsCloseCode.NORMAL);\n"
        "if (proto.onFrame(peerClose).getKind() != WsReadAction.CLOSED) return -1;\n"
        // a data frame after close is ignored: still CLOSED, no reply
        "WsFrame data = WsFrame.of(true, WsOpcode.TEXT, false, #p);\n"
        "WsReadAction a = proto.onFrame(data);\n"
        "if (a.getKind() != WsReadAction.CLOSED) return -2;\n"
        "if (a.getFrame() != null) return -3;\n"
        "if (a.getCloseReason().getCode() != 1000) return -4;\n"
        "return 1;"), 1);
}

// --- a malformed close payload (1 byte) propagates ProtocolViolation -----
TEST(WsProtocolTests, malformedCloseIsProtocolViolation) {
    EXPECT_EQ(runI32(
        "int8[] one = new int8[1];\n"
        "one[0] = (int8) 3;\n"
        "WsFrame bad = WsFrame.of(true, WsOpcode.CLOSE, false, #one);\n"
        "WsProtocol proto = WsProtocol.create();\n"
        "try {\n"
        "    WsReadAction a = proto.onFrame(bad);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- a continuation with no message open propagates ProtocolViolation ----
TEST(WsProtocolTests, orphanContinuationIsProtocolViolation) {
    EXPECT_EQ(runI32(
        emitAscii("p", "x") +
        "WsFrame orphan = WsFrame.of(true, WsOpcode.CONTINUATION, false, #p);\n"
        "WsProtocol proto = WsProtocol.create();\n"
        "try {\n"
        "    WsReadAction a = proto.onFrame(orphan);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- the per-message cap is enforced through the engine -----------------
TEST(WsProtocolTests, maxMessageLengthEnforced) {
    EXPECT_EQ(runI32(
        "int8[] big = new int8[10];\n"
        "int32 i = 0;\n"
        "while (i < 10) { big[i] = (int8) 65; i = i + 1; }\n"
        "WsProtocol proto = WsProtocol.create();\n"
        "proto.withMaxMessageLength((int64) 4);\n"
        "WsFrame f = WsFrame.of(true, WsOpcode.BINARY, false, #big);\n"
        "try {\n"
        "    WsReadAction a = proto.onFrame(f);\n"
        "    return -1;\n"
        "} catch (MessageTooLargeException e) {\n"
        "    return 1;\n"
        "}"), 1);
}
