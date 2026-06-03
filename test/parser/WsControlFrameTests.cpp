// NET-10.5 — cajeta.net.ws control frames (ping / pong / close).
//
// The control-frame layer is pure protocol logic over WsFrame + int8[]
// (no I/O): build a ping/pong/close frame, auto-pong a received ping by
// echoing its payload, parse a close frame into its { code, reason }, and
// build the reciprocal close for the bidirectional close handshake. So,
// exactly like WsFrameCodecTests (NET-10.3), each case compiles a small
// Cajeta run() that drives WsControlFrames / WsCloseReason / WsCloseCode
// and returns an int32 sentinel (positive on success, a distinct negative
// per failed sub-check).
//
// Pins the NET-10.5 deliverable: "Control frames: ping/pong (auto-pong
// unless the app opts to handle it), close handshake (close codes, reason,
// the bidirectional close exchange). Control frames interleaved between
// data fragments." (plan/cajeta-net-plan.md, Phase 10) and the acceptance
// rows closeHandshakeReportsCode / fragmentationWithInterleavedPing (the
// auto-pong half).

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
        "import cajeta.net.ws.WsFrameDecoder;\n"
        "import cajeta.net.ws.WsFrameEncoder;\n"
        "import cajeta.net.ws.WsCloseCode;\n"
        "import cajeta.net.ws.WsCloseReason;\n"
        "import cajeta.net.ws.WsControlFrames;\n"
        "import cajeta.net.ws.ProtocolViolationException;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- ping: opcode, FIN, control classification, payload ownership -------

TEST(WsControlFrameTests, pingCarriesPayloadAndIsControl) {
    EXPECT_EQ(runI32(
        "int8[] data = new int8[3];\n"
        "data[0] = (int8) 1;\n"
        "data[1] = (int8) 2;\n"
        "data[2] = (int8) 3;\n"
        "WsFrame f = WsControlFrames.ping(#data);\n"
        "if (f.getOpcode() != WsOpcode.PING) return -1;\n"
        "if (!f.isFin()) return -2;\n"
        "if (!f.isControl()) return -3;\n"
        "if (f.payloadLength() != 3) return -4;\n"
        "if (f.getPayload()[2] != (int8) 3) return -5;\n"
        "return 1;"), 1);
}

// A bare heartbeat ping (empty payload).
TEST(WsControlFrameTests, emptyPingIsValid) {
    EXPECT_EQ(runI32(
        "WsFrame f = WsControlFrames.ping(new int8[0]);\n"
        "if (f.getOpcode() != WsOpcode.PING) return -1;\n"
        "if (f.payloadLength() != 0) return -2;\n"
        "return 1;"), 1);
}

// A control payload > 125 bytes is rejected at build time.
TEST(WsControlFrameTests, oversizeControlPayloadRejected) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    WsFrame f = WsControlFrames.ping(new int8[126]);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- auto-pong: pongFor echoes the ping payload IDENTICALLY -------------
//
// RFC 6455 §5.5.3: a pong sent in response to a ping must carry the same
// application data. pongFor copies the ping payload into a fresh pong.
TEST(WsControlFrameTests, pongForEchoesPingPayload) {
    EXPECT_EQ(runI32(
        "int8[] data = new int8[5];\n"
        "data[0] = (int8) 72;\n"    // 'H'
        "data[1] = (int8) 101;\n"   // 'e'
        "data[2] = (int8) 108;\n"   // 'l'
        "data[3] = (int8) 108;\n"   // 'l'
        "data[4] = (int8) 111;\n"   // 'o'
        "WsFrame ping = WsControlFrames.ping(#data);\n"
        "WsFrame pong = WsControlFrames.pongFor(ping);\n"
        "if (pong.getOpcode() != WsOpcode.PONG) return -1;\n"
        "if (!pong.isFin()) return -2;\n"
        "if (pong.payloadLength() != 5) return -3;\n"
        "int8[] pp = pong.getPayload();\n"
        "if (pp[0] != (int8) 72) return -4;\n"
        "if (pp[4] != (int8) 111) return -5;\n"
        // The pong must own a DISTINCT buffer (a copy), not alias the ping.
        "int8[] ip = ping.getPayload();\n"
        "if (ip.count() != 5) return -6;\n"   // ping payload untouched
        "return 1;"), 1);
}

// An empty ping auto-pongs to an empty pong.
TEST(WsControlFrameTests, pongForEmptyPing) {
    EXPECT_EQ(runI32(
        "WsFrame ping = WsControlFrames.ping(new int8[0]);\n"
        "WsFrame pong = WsControlFrames.pongFor(ping);\n"
        "if (pong.getOpcode() != WsOpcode.PONG) return -1;\n"
        "if (pong.payloadLength() != 0) return -2;\n"
        "return 1;"), 1);
}

// pongFor rejects a non-ping frame.
TEST(WsControlFrameTests, pongForRejectsNonPing) {
    EXPECT_EQ(runI32(
        "WsFrame notPing = WsControlFrames.pong(new int8[0]);\n"
        "try {\n"
        "    WsFrame p = WsControlFrames.pongFor(notPing);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- close: build a close frame with code + reason ----------------------
//
// The close payload is [code-hi][code-lo][reason-utf8...]. A 1000 "bye"
// close is 88 05 03e8 627965 (after masking decisions).
TEST(WsControlFrameTests, closeBuildsCodeAndReason) {
    EXPECT_EQ(runI32(
        "String reason = \"bye\";\n"
        "WsFrame f = WsControlFrames.close(WsCloseCode.NORMAL, reason);\n"
        "if (f.getOpcode() != WsOpcode.CLOSE) return -1;\n"
        "if (!f.isFin()) return -2;\n"
        "if (f.payloadLength() != 5) return -3;\n"   // 2 code + 3 reason
        "int8[] p = f.getPayload();\n"
        "int32 code = ((((int32) p[0]) & 255) << 8) | (((int32) p[1]) & 255);\n"
        "if (code != 1000) return -4;\n"
        "if (p[2] != (int8) 98) return -5;\n"   // 'b'
        "if (p[3] != (int8) 121) return -6;\n"  // 'y'
        "if (p[4] != (int8) 101) return -7;\n"  // 'e'
        "return 1;"), 1);
}

// closeCode builds a 2-byte (code-only, no reason) close.
TEST(WsControlFrameTests, closeCodeNoReason) {
    EXPECT_EQ(runI32(
        "WsFrame f = WsControlFrames.closeCode(WsCloseCode.GOING_AWAY);\n"
        "if (f.payloadLength() != 2) return -1;\n"
        "int8[] p = f.getPayload();\n"
        "int32 code = ((((int32) p[0]) & 255) << 8) | (((int32) p[1]) & 255);\n"
        "if (code != 1001) return -2;\n"
        "return 1;"), 1);
}

// closeEmpty builds a zero-length close payload (RFC 6455 §5.5.1).
TEST(WsControlFrameTests, closeEmptyHasNoPayload) {
    EXPECT_EQ(runI32(
        "WsFrame f = WsControlFrames.closeEmpty();\n"
        "if (f.getOpcode() != WsOpcode.CLOSE) return -1;\n"
        "if (f.payloadLength() != 0) return -2;\n"
        "return 1;"), 1);
}

// A non-sendable close code (1005 / 1006 / 1015) is rejected at build.
TEST(WsControlFrameTests, closeRejectsReservedCode) {
    EXPECT_EQ(runI32(
        "try {\n"
        "    WsFrame f = WsControlFrames.closeCode(WsCloseCode.NO_STATUS);\n"   // 1005
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// A reason > 123 bytes overflows the 125-byte control cap → rejected.
TEST(WsControlFrameTests, closeRejectsOversizeReason) {
    EXPECT_EQ(runI32(
        // Build a 124-byte ASCII reason ('A' * 124) — 2 + 124 = 126 > 125.
        "int8[] rb = new int8[124];\n"
        "int32 i = 0;\n"
        "while (i < 124) { rb[i] = (int8) 65; i = i + 1; }\n"
        "String reason = heap String(#rb, 124);\n"
        "try {\n"
        "    WsFrame f = WsControlFrames.close(WsCloseCode.NORMAL, reason);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- parseClose: extract { code, reason } from a close frame ------------
//
// The close-handshake acceptance row closeHandshakeReportsCode: a received
// close frame must report its close code.
TEST(WsControlFrameTests, parseCloseExtractsCodeAndReason) {
    EXPECT_EQ(runI32(
        "String reason = \"done\";\n"
        "WsFrame f = WsControlFrames.close(WsCloseCode.NORMAL, reason);\n"
        "WsCloseReason r = WsControlFrames.parseClose(f);\n"
        "if (!r.hasCode()) return -1;\n"
        "if (r.getCode() != 1000) return -2;\n"
        "String back = r.getReason();\n"
        "if (back.byteLength != 4) return -3;\n"
        "if (back.bytes[0] != (int8) 100) return -4;\n"   // 'd'
        "if (back.bytes[3] != (int8) 101) return -5;\n"   // 'e'
        "return 1;"), 1);
}

// An empty close frame parses to the no-status (1005) placeholder.
TEST(WsControlFrameTests, parseCloseEmptyIsNoStatus) {
    EXPECT_EQ(runI32(
        "WsFrame f = WsControlFrames.closeEmpty();\n"
        "WsCloseReason r = WsControlFrames.parseClose(f);\n"
        "if (r.hasCode()) return -1;\n"
        "if (r.getCode() != WsCloseCode.NO_STATUS) return -2;\n"   // 1005
        "if (r.getReason().byteLength != 0) return -3;\n"
        "return 1;"), 1);
}

// A 1-byte close payload is malformed (a code must be a whole 2 bytes).
TEST(WsControlFrameTests, parseCloseOneBytePayloadRejected) {
    EXPECT_EQ(runI32(
        "int8[] one = new int8[1];\n"
        "one[0] = (int8) 3;\n"
        "WsFrame f = WsFrame.of(true, WsOpcode.CLOSE, false, #one);\n"
        "try {\n"
        "    WsCloseReason r = WsControlFrames.parseClose(f);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// A close frame whose code is one the peer MUST NOT send (1006) is a
// protocol error on parse.
TEST(WsControlFrameTests, parseCloseRejectsForbiddenWireCode) {
    EXPECT_EQ(runI32(
        // payload 03ee = 1006 (ABNORMAL) — never legal on the wire.
        "int8[] p = new int8[2];\n"
        "p[0] = (int8) 3;\n"
        "p[1] = (int8) -18;\n"   // 0xee -> 1006
        "WsFrame f = WsFrame.of(true, WsOpcode.CLOSE, false, #p);\n"
        "try {\n"
        "    WsCloseReason r = WsControlFrames.parseClose(f);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// parseClose rejects a non-close frame.
TEST(WsControlFrameTests, parseCloseRejectsNonCloseFrame) {
    EXPECT_EQ(runI32(
        "WsFrame ping = WsControlFrames.ping(new int8[0]);\n"
        "try {\n"
        "    WsCloseReason r = WsControlFrames.parseClose(ping);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- bidirectional close handshake: reciprocalClose ---------------------
//
// Receiving a peer's close, the endpoint replies with a close echoing the
// same code (RFC 6455 §5.5.1). The full exchange: build a close, parse it,
// build the reciprocal, and confirm the code round-trips.
TEST(WsControlFrameTests, reciprocalCloseEchoesPeerCode) {
    EXPECT_EQ(runI32(
        "WsFrame peer = WsControlFrames.closeCode(WsCloseCode.GOING_AWAY);\n"  // 1001
        "WsCloseReason r = WsControlFrames.parseClose(peer);\n"
        "WsFrame reply = WsControlFrames.reciprocalClose(r);\n"
        "if (reply.getOpcode() != WsOpcode.CLOSE) return -1;\n"
        "WsCloseReason rr = WsControlFrames.parseClose(reply);\n"
        "if (!rr.hasCode()) return -2;\n"
        "if (rr.getCode() != 1001) return -3;\n"
        "return 1;"), 1);
}

// A code-less peer close gets a NORMAL (1000) reciprocal.
TEST(WsControlFrameTests, reciprocalCloseForEmptyIsNormal) {
    EXPECT_EQ(runI32(
        "WsFrame peer = WsControlFrames.closeEmpty();\n"
        "WsCloseReason r = WsControlFrames.parseClose(peer);\n"
        "WsFrame reply = WsControlFrames.reciprocalClose(r);\n"
        "WsCloseReason rr = WsControlFrames.parseClose(reply);\n"
        "if (!rr.hasCode()) return -1;\n"
        "if (rr.getCode() != WsCloseCode.NORMAL) return -2;\n"   // 1000
        "return 1;"), 1);
}

// --- interleaving: a control frame round-trips through the codec --------
//
// The acceptance row fragmentationWithInterleavedPing: a ping interleaved
// mid-fragment is answered without corrupting the message. Here we pin the
// codec half — a built ping encodes + decodes back to a ping with the same
// payload, so the read loop can pull a control frame out from between data
// fragments and auto-pong it.
TEST(WsControlFrameTests, builtPingRoundTripsThroughCodec) {
    EXPECT_EQ(runI32(
        "int8[] data = new int8[4];\n"
        "data[0] = (int8) 222;\n"   // 0xde
        "data[1] = (int8) 173;\n"   // 0xad
        "data[2] = (int8) 190;\n"   // 0xbe
        "data[3] = (int8) 239;\n"   // 0xef
        "WsFrame ping = WsControlFrames.ping(#data);\n"
        "int8[] wire = WsFrameEncoder.encode(ping, null);\n"   // server: unmasked
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "dec.feed(wire, wire.count());\n"
        "if (!dec.hasFrame()) return -1;\n"
        "WsFrame g = dec.nextFrame();\n"
        "if (g.getOpcode() != WsOpcode.PING) return -2;\n"
        "if (!g.isControl()) return -3;\n"
        "if (g.payloadLength() != 4) return -4;\n"
        "WsFrame pong = WsControlFrames.pongFor(g);\n"
        "if (pong.getOpcode() != WsOpcode.PONG) return -5;\n"
        "if (pong.getPayload()[0] != (int8) 222) return -6;\n"
        "if (pong.getPayload()[3] != (int8) 239) return -7;\n"
        "return 1;"), 1);
}
