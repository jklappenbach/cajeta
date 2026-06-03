// NET-10.4 — cajeta.net.ws message fragmentation tests (RFC 6455 §5.4).
//
// The fragmentation reassembler (WsMessageAssembler) is a pure-logic state
// machine over decoded WsFrames — no I/O — so it tests directly over the
// JIT the same way WsFrameCodecTests drives the frame codec: each test
// compiles a small Cajeta `run()` that feeds frames to the assembler and
// returns an int32 sentinel (positive on success, a distinct negative per
// failed sub-check).
//
// Frames are constructed with WsFrame.of(fin, opcode, masked, payload)
// directly (the assembler consumes decoded frames, not wire bytes), except
// the end-to-end case which decodes wire bytes through WsFrameDecoder first
// to exercise the codec -> assembler hand-off and an interleaved control
// frame.
//
// Pins the NET-10.4 deliverable: "Message fragmentation: reassemble
// continuation frames into a logical message; enforce a max-message-size
// limit." (plan/cajeta-net-plan.md, Phase 10) and the fragmentation half
// of the acceptance row fragmentationWithInterleavedPing.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "import cajeta.net.ws.WsOpcode;\n"
        "import cajeta.net.ws.WsFrame;\n"
        "import cajeta.net.ws.WsMessage;\n"
        "import cajeta.net.ws.WsMessageAssembler;\n"
        "import cajeta.net.ws.WsFrameDecoder;\n"
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

// --- single unfragmented frame is a whole message ----------------------
//
// A lone FIN=1 text frame yields a message immediately, with the same
// opcode + payload (copied, so the message owns its bytes).
TEST(WsMessageAssemblerTests, singleFrameMessage) {
    EXPECT_EQ(runI32(
        emitAscii("p", "Hello") +
        "WsFrame f = WsFrame.of(true, WsOpcode.TEXT, false, #p);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "WsMessage m = asm.accept(f);\n"
        "if (m == null) return -1;\n"
        "if (!m.isText()) return -2;\n"
        "if (m.getOpcode() != WsOpcode.TEXT) return -3;\n"
        "if (m.length() != 5) return -4;\n"
        "int8[] b = m.getPayload();\n"
        "if (b[0] != (int8) 72) return -5;\n"    // 'H'
        "if (b[4] != (int8) 111) return -6;\n"   // 'o'
        "if (asm.isAssembling()) return -7;\n"
        "return 1;"), 1);
}

// --- three-fragment message reassembles in order -----------------------
//
// A FIN=0 binary "Wiki", a FIN=0 continuation "media", a FIN=1
// continuation " WS" -> one binary message "Wikimedia WS". The first two
// fragments return null; the last returns the whole message.
TEST(WsMessageAssemblerTests, threeFragmentReassembly) {
    EXPECT_EQ(runI32(
        emitAscii("p0", "Wiki") +
        emitAscii("p1", "media") +
        emitAscii("p2", " WS") +
        "WsFrame f0 = WsFrame.of(false, WsOpcode.BINARY, false, #p0);\n"
        "WsFrame f1 = WsFrame.of(false, WsOpcode.CONTINUATION, false, #p1);\n"
        "WsFrame f2 = WsFrame.of(true, WsOpcode.CONTINUATION, false, #p2);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "if (asm.accept(f0) != null) return -1;\n"
        "if (!asm.isAssembling()) return -2;\n"
        "if (asm.accept(f1) != null) return -3;\n"
        "WsMessage m = asm.accept(f2);\n"
        "if (m == null) return -4;\n"
        "if (asm.isAssembling()) return -5;\n"
        "if (!m.isBinary()) return -6;\n"
        "if (m.length() != 12) return -7;\n"           // "Wikimedia WS"
        "int8[] b = m.getPayload();\n"
        "if (b[0] != (int8) 87) return -8;\n"          // 'W'
        "if (b[4] != (int8) 109) return -9;\n"         // 'm' (start of "media")
        "if (b[11] != (int8) 83) return -10;\n"        // 'S' (last byte)
        "return 1;"), 1);
}

// --- empty / zero-length fragments ------------------------------------
//
// Fragments with empty payloads still reassemble; an all-empty message is
// a present-but-empty message.
TEST(WsMessageAssemblerTests, emptyFragmentsReassemble) {
    EXPECT_EQ(runI32(
        emitAscii("p0", "") +
        emitAscii("p1", "AB") +
        emitAscii("p2", "") +
        "WsFrame f0 = WsFrame.of(false, WsOpcode.TEXT, false, #p0);\n"
        "WsFrame f1 = WsFrame.of(false, WsOpcode.CONTINUATION, false, #p1);\n"
        "WsFrame f2 = WsFrame.of(true, WsOpcode.CONTINUATION, false, #p2);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "asm.accept(f0);\n"
        "asm.accept(f1);\n"
        "WsMessage m = asm.accept(f2);\n"
        "if (m == null) return -1;\n"
        "if (m.length() != 2) return -2;\n"
        "int8[] b = m.getPayload();\n"
        "if (b[0] != (int8) 65) return -3;\n"   // 'A'
        "if (b[1] != (int8) 66) return -4;\n"   // 'B'
        "return 1;"), 1);
}

// --- two consecutive messages reuse one assembler ----------------------
//
// After a message completes, the assembler resets cleanly and reassembles
// the next one independently.
TEST(WsMessageAssemblerTests, consecutiveMessages) {
    EXPECT_EQ(runI32(
        emitAscii("a", "one") +
        emitAscii("b0", "tw") +
        emitAscii("b1", "o") +
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "WsFrame fa = WsFrame.of(true, WsOpcode.TEXT, false, #a);\n"
        "WsMessage m1 = asm.accept(fa);\n"
        "if (m1 == null) return -1;\n"
        "if (m1.length() != 3) return -2;\n"
        "WsFrame fb0 = WsFrame.of(false, WsOpcode.BINARY, false, #b0);\n"
        "WsFrame fb1 = WsFrame.of(true, WsOpcode.CONTINUATION, false, #b1);\n"
        "if (asm.accept(fb0) != null) return -3;\n"
        "WsMessage m2 = asm.accept(fb1);\n"
        "if (m2 == null) return -4;\n"
        "if (!m2.isBinary()) return -5;\n"
        "if (m2.length() != 3) return -6;\n"
        "int8[] b = m2.getPayload();\n"
        "if (b[0] != (int8) 116) return -7;\n"   // 't'
        "if (b[2] != (int8) 111) return -8;\n"   // 'o'
        "return 1;"), 1);
}

// --- interleaved control frame mid-fragment (decoded from wire) --------
//
// A FIN=0 text "AB", then a ping (control) interleaved, then a FIN=1
// continuation "CD". The ping passes through (accept returns null, leaves
// the partial message intact); the continuation completes "ABCD".
TEST(WsMessageAssemblerTests, interleavedControlFrameDoesNotCorruptMessage) {
    EXPECT_EQ(runI32(
        // wire: 0102 4142  (FIN=0 text "AB")
        //       8900       (ping, empty)
        //       8002 4344  (FIN=1 continuation "CD")
        "int8[] wire = new int8[10];\n"
        "wire[0] = (int8) 1;\n"     // 0x01 FIN=0 text
        "wire[1] = (int8) 2;\n"     // len 2
        "wire[2] = (int8) 65;\n"    // 'A'
        "wire[3] = (int8) 66;\n"    // 'B'
        "wire[4] = (int8) -119;\n"   // 0x89 FIN=1 ping
        "wire[5] = (int8) 0;\n"     // len 0
        "wire[6] = (int8) -128;\n"   // 0x80 FIN=1 continuation
        "wire[7] = (int8) 2;\n"     // len 2
        "wire[8] = (int8) 67;\n"    // 'C'
        "wire[9] = (int8) 68;\n"    // 'D'
        "WsFrameDecoder dec = WsFrameDecoder.forClient();\n"
        "dec.feed(wire, 10);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "WsMessage done = null;\n"
        "int32 pings = 0;\n"
        "while (dec.hasFrame()) {\n"
        "    WsFrame f = dec.nextFrame();\n"
        "    if (f.getOpcode() == WsOpcode.PING) { pings = pings + 1; }\n"
        "    WsMessage m = asm.accept(f);\n"
        "    if (m != null) { done = m; }\n"
        "}\n"
        "if (pings != 1) return -1;\n"
        "if (done == null) return -2;\n"
        "if (!done.isText()) return -3;\n"
        "if (done.length() != 4) return -4;\n"          // "ABCD"
        "int8[] b = done.getPayload();\n"
        "if (b[0] != (int8) 65) return -5;\n"   // 'A'
        "if (b[1] != (int8) 66) return -6;\n"   // 'B'
        "if (b[2] != (int8) 67) return -7;\n"   // 'C'
        "if (b[3] != (int8) 68) return -8;\n"   // 'D'
        "return 1;"), 1);
}

// --- control frame returns null and is not reassembled -----------------
TEST(WsMessageAssemblerTests, controlFrameReturnsNull) {
    EXPECT_EQ(runI32(
        emitAscii("p", "") +
        "WsFrame ping = WsFrame.of(true, WsOpcode.PING, false, #p);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "WsMessage m = asm.accept(ping);\n"
        "if (m != null) return -1;\n"
        "if (asm.isAssembling()) return -2;\n"
        "return 1;"), 1);
}

// --- protocol violation: continuation with no message in progress ------
TEST(WsMessageAssemblerTests, danglingContinuationRejected) {
    EXPECT_EQ(runI32(
        emitAscii("p", "X") +
        "WsFrame cont = WsFrame.of(true, WsOpcode.CONTINUATION, false, #p);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "try {\n"
        "    asm.accept(cont);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- protocol violation: new data frame while a message is open --------
TEST(WsMessageAssemblerTests, interleavedDataFrameRejected) {
    EXPECT_EQ(runI32(
        emitAscii("p0", "AB") +
        emitAscii("p1", "CD") +
        "WsFrame open = WsFrame.of(false, WsOpcode.TEXT, false, #p0);\n"
        "WsFrame intr = WsFrame.of(false, WsOpcode.BINARY, false, #p1);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "if (asm.accept(open) != null) return -2;\n"
        "try {\n"
        "    asm.accept(intr);\n"
        "    return -1;\n"
        "} catch (ProtocolViolationException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- max-message-size limit: single oversized frame --------------------
//
// A lone frame whose payload exceeds the cap is MessageTooLarge.
TEST(WsMessageAssemblerTests, oversizeSingleFrameRejected) {
    EXPECT_EQ(runI32(
        "int8[] p = new int8[200];\n"
        "int32 i = 0;\n"
        "while (i < 200) { p[i] = (int8) 90; i = i + 1; }\n"
        "WsFrame f = WsFrame.of(true, WsOpcode.BINARY, false, #p);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "asm.withMaxMessageLength((int64) 100);\n"
        "try {\n"
        "    asm.accept(f);\n"
        "    return -1;\n"
        "} catch (MessageTooLargeException e) {\n"
        "    if (e.limit != ((int64) 100)) return -2;\n"
        "    return 1;\n"
        "}"), 1);
}

// --- max-message-size limit: accumulated fragments ---------------------
//
// Many small continuation frames whose running total crosses the cap are
// MessageTooLarge even though each frame is small (the per-message guard,
// distinct from the per-frame guard).
TEST(WsMessageAssemblerTests, accumulatedFragmentsExceedLimit) {
    EXPECT_EQ(runI32(
        "int8[] chunk = new int8[40];\n"
        "int32 i = 0;\n"
        "while (i < 40) { chunk[i] = (int8) 65; i = i + 1; }\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "asm.withMaxMessageLength((int64) 100);\n"
        // 40 + 40 = 80 (ok), + 40 = 120 (> 100) -> reject on the 3rd.
        "int8[] c0 = new int8[40];\n"
        "int8[] c1 = new int8[40];\n"
        "int8[] c2 = new int8[40];\n"
        "int32 j = 0;\n"
        "while (j < 40) { c0[j] = (int8) 65; c1[j] = (int8) 65; c2[j] = (int8) 65; j = j + 1; }\n"
        "WsFrame f0 = WsFrame.of(false, WsOpcode.BINARY, false, #c0);\n"
        "WsFrame f1 = WsFrame.of(false, WsOpcode.CONTINUATION, false, #c1);\n"
        "WsFrame f2 = WsFrame.of(true, WsOpcode.CONTINUATION, false, #c2);\n"
        "if (asm.accept(f0) != null) return -2;\n"
        "if (asm.accept(f1) != null) return -3;\n"
        "try {\n"
        "    asm.accept(f2);\n"
        "    return -1;\n"
        "} catch (MessageTooLargeException e) {\n"
        "    return 1;\n"
        "}"), 1);
}

// --- exactly-at-limit message is accepted ------------------------------
//
// A message whose total equals the cap exactly is fine (the limit is a
// ceiling, not a strict-less-than).
TEST(WsMessageAssemblerTests, exactlyAtLimitAccepted) {
    EXPECT_EQ(runI32(
        "int8[] p = new int8[100];\n"
        "int32 i = 0;\n"
        "while (i < 100) { p[i] = (int8) 88; i = i + 1; }\n"
        "WsFrame f = WsFrame.of(true, WsOpcode.BINARY, false, #p);\n"
        "WsMessageAssembler asm = WsMessageAssembler.create();\n"
        "asm.withMaxMessageLength((int64) 100);\n"
        "WsMessage m = asm.accept(f);\n"
        "if (m == null) return -1;\n"
        "if (m.length() != 100) return -2;\n"
        "return 1;"), 1);
}
