// NET-10.3 — cajeta.io.net.ws frame codec tests (RFC 6455 §5.2 framing).
//
// The WebSocket frame codec is a pair of pure-logic transforms over byte
// buffers — an incremental resumable decoder (WsFrameDecoder) and a
// serializer (WsFrameEncoder), no I/O — so it tests directly over the
// JIT exactly the way HttpParserTests / BodyReaderTests drive the HTTP
// codecs: each test compiles a small Cajeta `run()` that feeds wire bytes
// to the codec and returns an int32 sentinel (positive on success, a
// distinct negative per failed sub-check).
//
// Wire bytes here are arbitrary octets (masked frames carry non-ASCII),
// so — unlike the HTTP string-literal vectors — the bytes are emitted as
// an explicit `int8[]` built element-by-element from the golden corpus
// (see emitBytes). The vectors mirror test/net/golden/ws/frames.vectors
// byte-for-byte, the same RFC 6455 §5.7 corpus the NET-13.2 golden
// meta-test (GoldenVectorsTests.cpp) self-validates, so this suite is the
// executable half of that known-answer set.
//
// Pins the NET-10.3 deliverable: "Frame codec: encode/decode FIN + RSV +
// opcode + MASK + payload-length (7/16/64-bit forms) + masking key;
// client frames masked, server frames unmasked (RFC 6455 §5). Incremental
// decoder (frame split across reads)." (plan/cajeta-net-plan.md, Phase
// 10) and its acceptance row maskingEnforced.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "net/GoldenVectors.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.io.net.ws.WsOpcode;\n"
        "import cajeta.io.net.ws.WsFrame;\n"
        "import cajeta.io.net.ws.WsFrameDecoder;\n"
        "import cajeta.io.net.ws.WsFrameEncoder;\n"
        "import cajeta.io.net.ws.ProtocolViolationException;\n"
        "public final class M {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Compile-once form for the golden-vector batches: takes a class body that
// already declares its own methods + `run()`, compiles the whole corpus in a
// SINGLE stdlib JIT pass (instead of one ~8-15s compile per vector — the
// difference between ~120s and ~8s), and invokes run(). Mirrors the
// per-vector-helper-method shape UriResolveTests adopted (each vector in its
// own method so the JIT lowers them cleanly).
int32_t runFull(const std::string& classBody) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.io.net.ws.WsOpcode;\n"
        "import cajeta.io.net.ws.WsFrame;\n"
        "import cajeta.io.net.ws.WsFrameDecoder;\n"
        "import cajeta.io.net.ws.WsFrameEncoder;\n"
        "import cajeta.io.net.ws.ProtocolViolationException;\n"
        "public final class M {\n" + classBody + "}\n";
    auto jit = CajetaJit::compile(src, "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Emit Cajeta that declares `int8[] <name> = heap int8[N];` and fills it
// from `bytes`. Each byte is written as a signed int8 cast (a value >127
// becomes its two's-complement signed form, which `(int8) N` produces).
std::string emitBytes(const std::string& name, const std::vector<uint8_t>& bytes) {
    std::string s = "int8[] " + name + " = heap int8[" +
                    std::to_string(bytes.size()) + "];\n";
    for (size_t i = 0; i < bytes.size(); i++) {
        s += name + "[" + std::to_string(i) + "] = (int8) " +
             std::to_string((int)(int8_t)bytes[i]) + ";\n";
    }
    return s;
}

const char* opcodeConst(uint8_t op) {
    switch (op) {
        case 0x0: return "WsOpcode.CONTINUATION";
        case 0x1: return "WsOpcode.TEXT";
        case 0x2: return "WsOpcode.BINARY";
        case 0x8: return "WsOpcode.CLOSE";
        case 0x9: return "WsOpcode.PING";
        case 0xa: return "WsOpcode.PONG";
        default:  return "0";
    }
}

} // namespace

// --- decode: every golden vector decodes to its declared fields --------
//
// A masked vector is decoded with forServer() (client->server frames must
// be masked); an unmasked vector with forClient() (server->client must
// not be). Either way the decoded payload is the UNMASKED bytes.
TEST(WsFrameCodecTests, decodeGoldenVectors) {
    auto vectors = cajeta_golden::wsFrameVectors();
    ASSERT_GE(vectors.size(), 8u) << "expected the RFC 6455 §5.7 corpus";

    std::string methods, dispatch;
    for (size_t vi = 0; vi < vectors.size(); vi++) {
        const auto& v = vectors[vi];
        std::string body = emitBytes("wire", v.wire);
        body += std::string("WsFrameDecoder dec = WsFrameDecoder.") +
                (v.masked ? "forServer();\n" : "forClient();\n");
        body += "dec.feed(wire, wire.count());\n";
        body += "if (!dec.hasFrame()) return -1;\n";
        body += "WsFrame f = dec.nextFrame();\n";
        body += std::string("if (") + (v.fin ? "!f.isFin()" : "f.isFin()") +
                ") return -2;\n";
        body += std::string("if (f.getOpcode() != ") + opcodeConst(v.opcode) +
                ") return -3;\n";
        body += std::string("if (") + (v.masked ? "!f.isMasked()" : "f.isMasked()") +
                ") return -4;\n";
        body += "int8[] pay = f.getPayload();\n";
        body += "if (pay.count() != " + std::to_string(v.payload.size()) +
                ") return -5;\n";
        // Verify the unmasked payload byte-for-byte.
        for (size_t i = 0; i < v.payload.size(); i++) {
            body += "if (pay[" + std::to_string(i) + "] != (int8) " +
                    std::to_string((int)(int8_t)v.payload[i]) + ") return -6;\n";
        }
        body += "if (dec.hasFrame()) return -7;\n";   // exactly one frame
        body += "return 1;";
        // One helper method per vector (the JIT lowers per-method bodies
        // cleanly); run() dispatches and returns the 0-based index of the
        // first failing vector, or -1 when every vector passed.
        methods += "    static int32 d" + std::to_string(vi) + "() {\n" + body +
                   "\n    }\n";
        dispatch += "        if (M.d" + std::to_string(vi) + "() != 1) return " +
                    std::to_string((int) vi) + ";\n";
    }
    std::string cls = methods +
        "    public static int32 run() {\n" + dispatch +
        "        return -1;\n    }\n";
    EXPECT_EQ(runFull(cls), -1)
        << "a WS decode golden vector failed (run() returned its 0-based index)";
}

// --- encode: every golden vector re-serializes to its exact wire bytes -
//
// A masked vector is encoded with its on-wire masking key (extracted from
// the wire: bytes [2..6) after the 2-byte header — every masked vector in
// the corpus has a <=125 payload, so its header is exactly 2 bytes); an
// unmasked vector with a null key. The output must equal the wire bytes.
TEST(WsFrameCodecTests, encodeGoldenVectors) {
    auto vectors = cajeta_golden::wsFrameVectors();
    ASSERT_GE(vectors.size(), 8u);

    std::string methods, dispatch;
    for (size_t vi = 0; vi < vectors.size(); vi++) {
        const auto& v = vectors[vi];
        std::string body = emitBytes("payload", v.payload);
        body += std::string("WsFrame f = WsFrame.of(") +
                (v.fin ? "true" : "false") + ", " + opcodeConst(v.opcode) +
                ", " + (v.masked ? "true" : "false") + ", #payload);\n";
        if (v.masked) {
            // The mask key is wire bytes [2..6) for a small-payload frame.
            std::vector<uint8_t> key(v.wire.begin() + 2, v.wire.begin() + 6);
            body += emitBytes("key", key);
            body += "int8[] out = WsFrameEncoder.encode(f, key);\n";
        } else {
            body += "int8[] out = WsFrameEncoder.encode(f, null);\n";
        }
        body += "if (out.count() != " + std::to_string(v.wire.size()) +
                ") return -1;\n";
        for (size_t i = 0; i < v.wire.size(); i++) {
            body += "if (out[" + std::to_string(i) + "] != (int8) " +
                    std::to_string((int)(int8_t)v.wire[i]) + ") return -2;\n";
        }
        body += "return 1;";
        methods += "    static int32 e" + std::to_string(vi) + "() {\n" + body +
                   "\n    }\n";
        dispatch += "        if (M.e" + std::to_string(vi) + "() != 1) return " +
                    std::to_string((int) vi) + ";\n";
    }
    std::string cls = methods +
        "    public static int32 run() {\n" + dispatch +
        "        return -1;\n    }\n";
    EXPECT_EQ(runFull(cls), -1)
        << "a WS encode golden vector failed (run() returned its 0-based index)";
}

// --- encode/decode round-trip (unmasked) -------------------------------
//
// A server frame encoded then decoded by a client decoder yields the same
// fields + payload — the identity the codec must hold over arbitrary
// payloads.
TEST(WsFrameCodecTests, encodeDecodeRoundTripUnmasked) {
    EXPECT_EQ(runI32(
        "String s = \"round-trip payload\";\n"
        "int32 n = s.byteLength();\n"
        // Copy the string's bytes into a fresh array we can hand off (the
        // frame takes ownership of its payload; never steal s.bytes).
        "int8[] p = heap int8[n];\n"
        "int32 j = 0;\n"
        "while (j < n) { p[j] = s.byteAt(j); j = j + 1; }\n"
        "WsFrame f = WsFrame.of(true, WsOpcode.BINARY, false, #p);\n"
        "int8[] wire = WsFrameEncoder.encode(f, null);\n"
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "dec.feed(wire, wire.count());\n"
        "if (!dec.hasFrame()) return -1;\n"
        "WsFrame g = dec.nextFrame();\n"
        "if (!g.isFin()) return -2;\n"
        "if (g.getOpcode() != WsOpcode.BINARY) return -3;\n"
        "if (g.isMasked()) return -4;\n"
        "int8[] back = g.getPayload();\n"
        "if (back.count() != n) return -5;\n"
        "int32 i = 0;\n"
        "while (i < n) {\n"
        "    if (back[i] != s.byteAt(i)) return -6;\n"
        "    i = i + 1;\n"
        "}\n"
        "return 1;"), 1);
}

// --- masking enforcement (the acceptance row maskingEnforced) ----------
//
// A masked client frame unmasks correctly; an UNMASKED frame fed to a
// server decoder is a ProtocolViolation.
TEST(WsFrameCodecTests, maskedClientFrameUnmasksCorrectly) {
    // The RFC 6455 §5.7 masked "Hello": 81 85 37fa213d 7f9f4d5158.
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[11];\n"
        "wire[0] = (int8) -127;\n"   // 0x81 FIN+text
        "wire[1] = (int8) -123;\n"   // 0x85 MASK + len 5
        "wire[2] = (int8) 55;\n"    // 0x37 mask
        "wire[3] = (int8) -6;\n"   // 0xfa
        "wire[4] = (int8) 33;\n"    // 0x21
        "wire[5] = (int8) 61;\n"    // 0x3d
        "wire[6] = (int8) 127;\n"   // 0x7f
        "wire[7] = (int8) -97;\n"   // 0x9f
        "wire[8] = (int8) 77;\n"    // 0x4d
        "wire[9] = (int8) 81;\n"    // 0x51
        "wire[10] = (int8) 88;\n"   // 0x58
        "WsFrameDecoder dec = WsFrameDecoder.forServer();\n"
        "dec.feed(wire, 11);\n"
        "WsFrame f = dec.nextFrame();\n"
        "int8[] p = f.getPayload();\n"
        "if (p.count() != 5) return -1;\n"
        "if (p[0] != (int8) 72) return -2;\n"    // 'H'
        "if (p[1] != (int8) 101) return -3;\n"   // 'e'
        "if (p[2] != (int8) 108) return -4;\n"   // 'l'
        "if (p[3] != (int8) 108) return -5;\n"   // 'l'
        "if (p[4] != (int8) 111) return -6;\n"   // 'o'
        "return 1;"), 1);
}

TEST(WsFrameCodecTests, unmaskedClientFrameIsProtocolViolation) {
    // Unmasked "Hello" text frame fed to a SERVER decoder: must reject.
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[7];\n"
        "wire[0] = (int8) -127;\n"   // 0x81
        "wire[1] = (int8) 5;\n"     // len 5, MASK clear
        "wire[2] = (int8) 72;\n"    // 'H'
        "wire[3] = (int8) 101;\n"
        "wire[4] = (int8) 108;\n"
        "wire[5] = (int8) 108;\n"
        "wire[6] = (int8) 111;\n"
        "WsFrameDecoder dec = WsFrameDecoder.forServer();\n"
        "try {\n"
        "    dec.feed(wire, 7);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

TEST(WsFrameCodecTests, maskedServerFrameIsProtocolViolation) {
    // A masked frame fed to a CLIENT decoder (server->client must NOT be
    // masked) is a violation.
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[7];\n"
        "wire[0] = (int8) -127;\n"
        "wire[1] = (int8) -126;\n"   // 0x82 MASK + len 2
        "wire[2] = (int8) 1;\n"     // mask key
        "wire[3] = (int8) 2;\n"
        "wire[4] = (int8) 3;\n"
        "wire[5] = (int8) 4;\n"
        "wire[6] = (int8) 9;\n"     // 1 masked payload byte... (partial ok, b1 already rejects)
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "try {\n"
        "    dec.feed(wire, 7);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- incremental decode: split feed == one shot ------------------------
//
// The masked "Hello" frame fed one byte at a time decodes identically to
// a one-shot feed — the resumable state machine resumes across every
// split (byte0, byte1, the 4 mask bytes, each payload byte).
TEST(WsFrameCodecTests, splitFeedMatchesOneShot) {
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[11];\n"
        "wire[0] = (int8) -127;\n"
        "wire[1] = (int8) -123;\n"
        "wire[2] = (int8) 55;\n"
        "wire[3] = (int8) -6;\n"
        "wire[4] = (int8) 33;\n"
        "wire[5] = (int8) 61;\n"
        "wire[6] = (int8) 127;\n"
        "wire[7] = (int8) -97;\n"
        "wire[8] = (int8) 77;\n"
        "wire[9] = (int8) 81;\n"
        "wire[10] = (int8) 88;\n"
        "WsFrameDecoder dec = WsFrameDecoder.forServer();\n"
        "int32 i = 0;\n"
        "int8[] one = heap int8[1];\n"
        "while (i < 11) {\n"
        "    one[0] = wire[i];\n"
        "    boolean had = dec.hasFrame();\n"
        "    dec.feed(one, 1);\n"
        "    i = i + 1;\n"
        "}\n"
        "if (!dec.hasFrame()) return -1;\n"
        "WsFrame f = dec.nextFrame();\n"
        "int8[] p = f.getPayload();\n"
        "if (p.count() != 5) return -2;\n"
        "if (p[0] != (int8) 72) return -3;\n"    // 'H'
        "if (p[4] != (int8) 111) return -4;\n"   // 'o'
        "return 1;"), 1);
}

// --- two frames in one feed (FIFO drain) -------------------------------
//
// A fragmentation pair (binary fragment FIN=0, then a continuation FIN=1)
// concatenated in one feed decodes to two frames in order — the queue
// hands them back FIFO.
TEST(WsFrameCodecTests, multipleFramesInOneFeedDrainFifo) {
    // 0203414243  (FIN=0 binary "ABC") then 8003444546 (FIN=1 cont "DEF").
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[10];\n"
        "wire[0] = (int8) 2;\n"     // 0x02 FIN=0 binary
        "wire[1] = (int8) 3;\n"     // len 3
        "wire[2] = (int8) 65;\n"    // 'A'
        "wire[3] = (int8) 66;\n"    // 'B'
        "wire[4] = (int8) 67;\n"    // 'C'
        "wire[5] = (int8) -128;\n"   // 0x80 FIN=1 continuation
        "wire[6] = (int8) 3;\n"     // len 3
        "wire[7] = (int8) 68;\n"    // 'D'
        "wire[8] = (int8) 69;\n"    // 'E'
        "wire[9] = (int8) 70;\n"    // 'F'
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "dec.feed(wire, 10);\n"
        "if (dec.frameCount() != 2) return -1;\n"
        "WsFrame f1 = dec.nextFrame();\n"
        "if (f1.isFin()) return -2;\n"                       // first: FIN=0
        "if (f1.getOpcode() != WsOpcode.BINARY) return -3;\n"
        "if (f1.payloadLength() != 3) return -4;\n"
        "if (f1.getPayload()[0] != (int8) 65) return -5;\n"  // 'A'
        "WsFrame f2 = dec.nextFrame();\n"
        "if (!f2.isFin()) return -6;\n"                      // second: FIN=1
        "if (f2.getOpcode() != WsOpcode.CONTINUATION) return -7;\n"
        "if (f2.getPayload()[2] != (int8) 70) return -8;\n"  // 'F'
        "if (dec.hasFrame()) return -9;\n"
        "return 1;"), 1);
}

// --- 16-bit extended length --------------------------------------------
//
// A 256-byte payload uses the 126 length form with a 2-byte big-endian
// length (0x0100). Decoded payload length is 256.
TEST(WsFrameCodecTests, sixteenBitLengthDecodes) {
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[260];\n"
        "wire[0] = (int8) -127;\n"   // 0x81 FIN+text
        "wire[1] = (int8) 126;\n"   // 126 -> 16-bit length follows
        "wire[2] = (int8) 1;\n"     // 0x01 high byte
        "wire[3] = (int8) 0;\n"     // 0x00 low byte -> 256
        "int32 i = 0;\n"
        "while (i < 256) {\n"
        "    wire[4 + i] = (int8) 65;\n"   // 'A'
        "    i = i + 1;\n"
        "}\n"
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "dec.feed(wire, 260);\n"
        "if (!dec.hasFrame()) return -1;\n"
        "WsFrame f = dec.nextFrame();\n"
        "if (f.payloadLength() != 256) return -2;\n"
        "if (f.getPayload()[0] != (int8) 65) return -3;\n"
        "if (f.getPayload()[255] != (int8) 65) return -4;\n"
        "return 1;"), 1);
}

// A 256-byte payload encodes to the 126 form with a 0x0100 length.
TEST(WsFrameCodecTests, sixteenBitLengthEncodes) {
    EXPECT_EQ(runI32(
        "int8[] p = heap int8[256];\n"
        "int32 i = 0;\n"
        "while (i < 256) { p[i] = (int8) 90; i = i + 1; }\n"   // 'Z'
        "WsFrame f = WsFrame.of(true, WsOpcode.TEXT, false, #p);\n"
        "int8[] out = WsFrameEncoder.encode(f, null);\n"
        "if (out.count() != 260) return -1;\n"            // 4 header + 256
        "if (out[0] != (int8) -127) return -2;\n"          // 0x81
        "if (out[1] != (int8) 126) return -3;\n"          // 126 marker
        "if (out[2] != (int8) 1) return -4;\n"            // 0x01
        "if (out[3] != (int8) 0) return -5;\n"            // 0x00
        "if (out[4] != (int8) 90) return -6;\n"           // first payload 'Z'
        "return 1;"), 1);
}

// --- 64-bit extended length --------------------------------------------
//
// The 127 length form with an 8-byte big-endian length is decoded; a
// payload of 70000 bytes (> 65535) exercises the 64-bit path.
TEST(WsFrameCodecTests, sixtyFourBitLengthRoundTrips) {
    EXPECT_EQ(runI32(
        "int8[] p = heap int8[70000];\n"
        "int32 i = 0;\n"
        "while (i < 70000) { p[i] = (int8) 88; i = i + 1; }\n"   // 'X'
        "WsFrame f = WsFrame.of(true, WsOpcode.BINARY, false, #p);\n"
        "int8[] out = WsFrameEncoder.encode(f, null);\n"
        "if (out.count() != 70010) return -1;\n"          // 10 header + 70000
        "if (out[1] != (int8) 127) return -2;\n"          // 127 marker
        // 70000 = 0x00011170; big-endian 8 bytes: 00 00 00 00 00 01 11 70
        "if (out[2] != (int8) 0) return -3;\n"
        "if (out[7] != (int8) 1) return -4;\n"
        "if (out[8] != (int8) 17) return -5;\n"           // 0x11
        "if (out[9] != (int8) 112) return -6;\n"          // 0x70
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "dec.feed(out, out.count());\n"
        "if (!dec.hasFrame()) return -7;\n"
        "WsFrame g = dec.nextFrame();\n"
        "if (g.payloadLength() != 70000) return -8;\n"
        "if (g.getPayload()[69999] != (int8) 88) return -9;\n"
        "return 1;"), 1);
}

// --- protocol violations: reserved opcode / RSV / fragmented control ----

TEST(WsFrameCodecTests, reservedOpcodeRejected) {
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[2];\n"
        "wire[0] = (int8) -125;\n"   // 0x83 FIN + reserved opcode 0x3
        "wire[1] = (int8) 0;\n"
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "try {\n"
        "    dec.feed(wire, 2);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

TEST(WsFrameCodecTests, rsvBitSetRejected) {
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[2];\n"
        "wire[0] = (int8) -63;\n"   // 0xC1 FIN + RSV1 + text
        "wire[1] = (int8) 0;\n"
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "try {\n"
        "    dec.feed(wire, 2);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

TEST(WsFrameCodecTests, fragmentedControlFrameRejected) {
    // FIN=0 on a ping (0x9) control opcode: 0x09 ... must reject.
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[2];\n"
        "wire[0] = (int8) 9;\n"     // 0x09 FIN=0 + ping
        "wire[1] = (int8) 0;\n"
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "try {\n"
        "    dec.feed(wire, 2);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

TEST(WsFrameCodecTests, oversizeControlFrameRejected) {
    // A ping (0x89) with the 126 extended-length form: control payload
    // > 125 is illegal regardless of the actual value.
    EXPECT_EQ(runI32(
        "int8[] wire = heap int8[4];\n"
        "wire[0] = (int8) -119;\n"   // 0x89 FIN + ping
        "wire[1] = (int8) 126;\n"   // 126 -> would be a 16-bit length
        "wire[2] = (int8) 0;\n"
        "wire[3] = (int8) 126;\n"
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "try {\n"
        "    dec.feed(wire, 4);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- control frames: empty ping/pong + close with a code ----------------

TEST(WsFrameCodecTests, emptyPingPongAndCloseDecode) {
    EXPECT_EQ(runI32(
        // ping-empty 8900, pong-empty 8a00, close-normal 880203e8
        "int8[] wire = heap int8[8];\n"
        "wire[0] = (int8) -119;\n"   // 0x89 ping
        "wire[1] = (int8) 0;\n"
        "wire[2] = (int8) -118;\n"   // 0x8a pong
        "wire[3] = (int8) 0;\n"
        "wire[4] = (int8) -120;\n"   // 0x88 close
        "wire[5] = (int8) 2;\n"     // len 2
        "wire[6] = (int8) 3;\n"     // 0x03
        "wire[7] = (int8) -24;\n"   // 0xe8 -> 1000
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "dec.feed(wire, 8);\n"
        "if (dec.frameCount() != 3) return -1;\n"
        "WsFrame ping = dec.nextFrame();\n"
        "if (ping.getOpcode() != WsOpcode.PING) return -2;\n"
        "if (ping.payloadLength() != 0) return -3;\n"
        "if (!ping.isControl()) return -4;\n"
        "WsFrame pong = dec.nextFrame();\n"
        "if (pong.getOpcode() != WsOpcode.PONG) return -5;\n"
        "WsFrame close = dec.nextFrame();\n"
        "if (close.getOpcode() != WsOpcode.CLOSE) return -6;\n"
        "if (close.payloadLength() != 2) return -7;\n"
        "int8[] cp = close.getPayload();\n"
        "int32 code = ((((int32) cp[0]) & 255) << 8) | (((int32) cp[1]) & 255);\n"
        "if (code != 1000) return -8;\n"
        "return 1;"), 1);
}

// --- frame-size guard ---------------------------------------------------
//
// A frame whose declared length exceeds the configured per-frame cap is a
// ProtocolViolation rather than an unbounded allocation.
TEST(WsFrameCodecTests, oversizeFrameExceedsCapRejected) {
    EXPECT_EQ(runI32(
        // 126 form with length 0x0100 = 256, cap set to 100.
        "int8[] wire = heap int8[4];\n"
        "wire[0] = (int8) -127;\n"   // 0x81 text
        "wire[1] = (int8) 126;\n"   // 16-bit length
        "wire[2] = (int8) 1;\n"     // 0x0100 = 256
        "wire[3] = (int8) 0;\n"
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "dec.withMaxFrameLength(100);\n"
        "try {\n"
        "    dec.feed(wire, 4);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}
