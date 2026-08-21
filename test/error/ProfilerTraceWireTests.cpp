// cajeta-profiler Unit 5 — protobuf wire primitives (plan 5.2.a, and the
// encoding half of 5.1.a).
//
// These are verified by ROUND-TRIP, which is a real check: an encoder and a
// decoder written from the same wrong understanding of base-128 varints would
// still disagree on the boundary cases below, because the byte counts are fixed
// by the format rather than by our convention.
//
// What round-trip does NOT establish is schema conformance — whether field 60
// really carries a TrackDescriptor. That is a separate concern and cannot be
// settled on this machine: there is no protoc, no trace_processor, and no
// vendored perfetto_trace.proto here.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include <cstdint>
#include <vector>
using cajeta_test::CajetaJit;

namespace {
struct Wire {
    std::unique_ptr<CajetaJit> jit;
    int32_t  (*wr)(uint8_t*, uint64_t) = nullptr;
    uint64_t (*rd)(const uint8_t*, int32_t, int32_t*) = nullptr;
    int32_t  (*tag)(uint8_t*, uint32_t, uint32_t) = nullptr;
    int32_t  (*u64)(uint8_t*, uint32_t, uint64_t) = nullptr;
    int32_t  (*bytes)(uint8_t*, uint32_t, const uint8_t*, int32_t) = nullptr;
};
Wire& wire() {
    static Wire w = [] {
        Wire x;
        x.jit = CajetaJit::compile(
            "package test;\npublic final class W {\n"
            "    public static int32 run() { return 1; }\n}\n", "test.W");
        auto s = [&](const char* n) { return x.jit->lookupRawSymbol(n); };
        x.wr    = reinterpret_cast<int32_t (*)(uint8_t*, uint64_t)>(s("__cajeta_pb_varint"));
        x.rd    = reinterpret_cast<uint64_t (*)(const uint8_t*, int32_t, int32_t*)>(s("__cajeta_pb_varint_read"));
        x.tag   = reinterpret_cast<int32_t (*)(uint8_t*, uint32_t, uint32_t)>(s("__cajeta_pb_tag"));
        x.u64   = reinterpret_cast<int32_t (*)(uint8_t*, uint32_t, uint64_t)>(s("__cajeta_pb_uint64"));
        x.bytes = reinterpret_cast<int32_t (*)(uint8_t*, uint32_t, const uint8_t*, int32_t)>(s("__cajeta_pb_bytes"));
        return x;
    }();
    return w;
}
} // namespace

TEST(ProfilerTraceWire, entryPointsResolve) {
    auto& w = wire();
    ASSERT_NE(w.wr, nullptr)    << "__cajeta_pb_varint unresolved";
    ASSERT_NE(w.rd, nullptr)    << "__cajeta_pb_varint_read unresolved";
    ASSERT_NE(w.tag, nullptr)   << "__cajeta_pb_tag unresolved";
    ASSERT_NE(w.u64, nullptr)   << "__cajeta_pb_uint64 unresolved";
    ASSERT_NE(w.bytes, nullptr) << "__cajeta_pb_bytes unresolved";
}

// Byte counts are fixed by the format, not by our convention: 127 must be one
// byte and 128 must be two. An encoder that merely round-trips with a matching
// decoder can still fail these.
TEST(ProfilerTraceWire, varintBoundariesHaveTheFormatsByteCounts) {
    auto& w = wire();
    ASSERT_NE(w.wr, nullptr);
    uint8_t b[16];
    EXPECT_EQ(w.wr(b, 0), 1);
    EXPECT_EQ(b[0], 0x00);
    EXPECT_EQ(w.wr(b, 127), 1);
    EXPECT_EQ(b[0], 0x7F);
    EXPECT_EQ(w.wr(b, 128), 2);
    EXPECT_EQ(b[0], 0x80);
    EXPECT_EQ(b[1], 0x01);
    EXPECT_EQ(w.wr(b, 300), 2);          // the protobuf spec's own example
    EXPECT_EQ(b[0], 0xAC);
    EXPECT_EQ(b[1], 0x02);
    EXPECT_EQ(w.wr(b, UINT64_MAX), 10);  // widest legal varint
}

TEST(ProfilerTraceWire, varintRoundTrips) {
    auto& w = wire();
    ASSERT_NE(w.wr, nullptr);
    const uint64_t vals[] = {0, 1, 2, 126, 127, 128, 129, 255, 256, 300,
                             16383, 16384, 1u << 21, 1ull << 31, 1ull << 32,
                             1ull << 63, UINT64_MAX - 1, UINT64_MAX};
    for (uint64_t v : vals) {
        uint8_t b[16] = {0};
        int32_t n = w.wr(b, v);
        int32_t consumed = 0;
        uint64_t back = w.rd(b, n, &consumed);
        EXPECT_EQ(back, v) << "round-trip failed for " << v;
        EXPECT_EQ(consumed, n) << "consumed != written for " << v;
    }
}

// 5.1.b's mechanism. A trace truncated mid-write ends inside a varint; the
// reader must report that rather than returning a plausible small number, or a
// truncated trace silently becomes a wrong trace.
TEST(ProfilerTraceWire, truncatedVarintIsDetectedNotGuessed) {
    auto& w = wire();
    ASSERT_NE(w.wr, nullptr);
    uint8_t b[16] = {0};
    int32_t n = w.wr(b, 1ull << 40);         // multi-byte
    ASSERT_GT(n, 2);
    for (int32_t cut = 1; cut < n; cut++) {  // every mid-varint truncation
        int32_t consumed = 0;
        uint64_t v = w.rd(b, cut, &consumed);
        EXPECT_EQ(consumed, -1) << "truncation at " << cut << " read as complete";
        EXPECT_EQ(v, 0u);
    }
}

// A tag is varint((field << 3) | wire). Field 1 / wire 2 is the packet framing
// every .pftrace begins with, so its exact byte is worth pinning.
TEST(ProfilerTraceWire, tagEncodesFieldAndWireType) {
    auto& w = wire();
    ASSERT_NE(w.tag, nullptr);
    uint8_t b[16] = {0};
    EXPECT_EQ(w.tag(b, 1, 2), 1);
    EXPECT_EQ(b[0], 0x0A) << "Trace.packet framing byte";
    EXPECT_EQ(w.tag(b, 8, 0), 1);
    EXPECT_EQ(b[0], 0x40);
    EXPECT_EQ(w.tag(b, 60, 2), 2) << "field 60 needs a two-byte tag";
}

// Length-delimited payloads: a string and a submessage are identical on the
// wire, which is what lets a submessage be built in scratch and appended
// without re-encoding.
TEST(ProfilerTraceWire, lengthDelimitedCarriesPayloadVerbatim) {
    auto& w = wire();
    ASSERT_NE(w.bytes, nullptr);
    const char* msg = "test.App";
    uint8_t b[64] = {0};
    int32_t n = w.bytes(b, 2, reinterpret_cast<const uint8_t*>(msg), 8);
    EXPECT_EQ(n, 1 + 1 + 8);
    EXPECT_EQ(b[0], 0x12);              // field 2, wire 2
    EXPECT_EQ(b[1], 8);                 // length
    EXPECT_EQ(memcmp(b + 2, msg, 8), 0);

    uint8_t u[16] = {0};
    int32_t un = w.u64(u, 8, 300);      // TracePacket.timestamp shape
    EXPECT_EQ(un, 3);
    EXPECT_EQ(u[0], 0x40);
    int32_t consumed = 0;
    EXPECT_EQ(w.rd(u + 1, 2, &consumed), 300u);
}
