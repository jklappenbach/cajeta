//
// VEA-5 — the downstream proof: cajeta-gossip's wire format, exactly as its
// spec declares it (specs/view-element-arrays-spec.md §2.1), compiles and a
// golden SWIM-shaped datagram round-trips: construct → header reads →
// per-delta reads (iterated via count(), the receive-loop pattern) →
// payload. @BigEndian throughout — exercises VEA-4's prefix swaps end to
// end. The Cajeta-side writer (W.u32be/u64be/...) is the shape gossip's
// encode path uses (var-size view fields are not assignable — encode is
// writer-side by design).
//
// Frame layout (123 bytes):
//   GossipMessage: magic u32 | version u8 | msgType u8 | senderInc u64
//                | senderName (u32+11) | deltas (u32 count + 2 × 42)
//                | payload (u32 + 2)
//   Delta (42):   state u8 | incarnation u64 | addrHi u64 | addrLo u64
//                | port u16 | name (u32+11)
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.G");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(ViewElementArrayGossipTests, gossipFrameRoundTrips) {
    auto src = std::string(
        "package test;\n"
        "@BigEndian\n"
        "public view Delta {\n"
        "    int8   state;\n"
        "    int64  incarnation;\n"
        "    int64  addrHi;\n"
        "    int64  addrLo;\n"
        "    uint16 port;\n"
        "    String name;\n"
        "}\n"
        "@BigEndian\n"
        "public view GossipMessage {\n"
        "    int32   magic;\n"
        "    int8    version;\n"
        "    int8    msgType;\n"
        "    int64   senderIncarnation;\n"
        "    String  senderName;\n"
        "    Delta[] deltas;\n"
        "    int8[]  payload;\n"
        "}\n"
        "public final class W {\n"
        "    public static int32 u32be(int8[] b, int32 o, int64 v) {\n"
        "        b[o]   = (int8) ((v >> 24) & 255);\n"
        "        b[o+1] = (int8) ((v >> 16) & 255);\n"
        "        b[o+2] = (int8) ((v >> 8) & 255);\n"
        "        b[o+3] = (int8) (v & 255);\n"
        "        return o + 4;\n"
        "    }\n"
        "    public static int32 u64be(int8[] b, int32 o, int64 v) {\n"
        "        int32 p = W.u32be(b, o, (v >> 32) & 4294967295);\n"
        "        return W.u32be(b, p, v & 4294967295);\n"
        "    }\n"
        "    public static int32 u16be(int8[] b, int32 o, int64 v) {\n"
        "        b[o]   = (int8) ((v >> 8) & 255);\n"
        "        b[o+1] = (int8) (v & 255);\n"
        "        return o + 2;\n"
        "    }\n"
        "    public static int32 str(int8[] b, int32 o, String s) {\n"
        "        int32 p = W.u32be(b, o, s.byteLength());\n"
        "        int32 i = 0;\n"
        "        while (i < s.byteLength()) {\n"
        "            b[p + i] = s.byteAt(i);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return p + s.byteLength();\n"
        "    }\n"
        "    public static int32 delta(int8[] b, int32 o, int64 st,\n"
        "            int64 inc, int64 hi, int64 lo, int64 port, String nm) {\n"
        "        b[o] = (int8) st;\n"
        "        int32 p = W.u64be(b, o + 1, inc);\n"
        "        p = W.u64be(b, p, hi);\n"
        "        p = W.u64be(b, p, lo);\n"
        "        p = W.u16be(b, p, port);\n"
        "        return W.str(b, p, nm);\n"
        "    }\n"
        "}\n"
        "public final class G {\n"
        "    public static int32 run() {\n"
        "        int8[] b = heap int8[123];\n"
        "        int32 p = W.u32be(b, 0, 1196582736);\n"     // magic 'GSWP'
        "        b[p] = (int8) 1;\n"                          // version
        "        b[p+1] = (int8) 2;\n"                        // msgType PING_REQ
        "        p = W.u64be(b, p + 2, 42);\n"                // senderInc
        "        p = W.str(b, p, \"node-a:7946\");\n"
        "        p = W.u32be(b, p, 2);\n"                     // delta count
        "        p = W.delta(b, p, 0, 7, 0, 2130706433, 7947, \"node-b:7947\");\n"
        "        p = W.delta(b, p, 1, 9, 0, 2130706434, 7948, \"node-c:7948\");\n"
        "        p = W.u32be(b, p, 2);\n"                     // payload len
        "        b[p] = (int8) 55;\n"
        "        b[p+1] = (int8) 66;\n"
        "        if (p + 2 != 123) return 20;\n"              // layout sanity
        "\n"
        "        GossipMessage m = GossipMessage(b);\n"
        "        if (m.magic != 1196582736) return 10;\n"
        "        if (m.version != 1) return 11;\n"
        "        if (m.msgType != 2) return 12;\n"
        "        if (m.senderIncarnation != 42) return 13;\n"
        "        String sn = m.senderName;\n"
        "        if (!sn.equals(\"node-a:7946\")) return 14;\n"
        "        if (m.deltas.count() != 2) return 15;\n"
        "\n"
        "        int64 incSum = 0;\n"                         // receive-loop pattern
        "        int32 i = 0;\n"
        "        while (i < 2) {\n"
        "            Delta d = m.deltas[i];\n"
        "            incSum = incSum + d.incarnation;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (incSum != 16) return 16;\n"
        "        if (m.deltas[0].state != 0) return 17;\n"
        "        if (m.deltas[1].state != 1) return 18;\n"
        "        if (m.deltas[1].addrLo != 2130706434) return 19;\n"
        "        if (m.deltas[0].port != 7947) return 21;\n"
        "        String n1 = m.deltas[1].name;\n"
        "        if (!n1.equals(\"node-c:7948\")) return 22;\n"
        "        int8[] pl = m.payload;\n"
        "        if (pl.count() != 2) return 23;\n"
        "        if (pl[0] != 55) return 24;\n"
        "        if (pl[1] != 66) return 25;\n"
        "        return 1;\n"
        "    }\n"
        "}\n");
    EXPECT_EQ(runI32(src), 1);
}
