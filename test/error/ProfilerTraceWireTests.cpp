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

// ── 5.2.d: the interning table ────────────────────────────────────────────
namespace {
struct Writer {
    int32_t  (*wsize)(void) = nullptr;
    int32_t  (*open)(void*, const char*) = nullptr;
    uint64_t (*intern)(void*, const char*) = nullptr;
    int32_t  (*close)(void*) = nullptr;
    int64_t  (*packets)(void*) = nullptr;
    int32_t  (*interned)(void*) = nullptr;
};
Writer wr() {
    auto& w = wire();
    Writer x;
    auto s = [&](const char* n) { return w.jit->lookupRawSymbol(n); };
    x.wsize    = reinterpret_cast<int32_t (*)(void)>(s("__cajeta_prof_trace_writer_size"));
    x.open     = reinterpret_cast<int32_t (*)(void*, const char*)>(s("__cajeta_prof_trace_open"));
    x.intern   = reinterpret_cast<uint64_t (*)(void*, const char*)>(s("__cajeta_prof_intern"));
    x.close    = reinterpret_cast<int32_t (*)(void*)>(s("__cajeta_prof_trace_close"));
    x.packets  = reinterpret_cast<int64_t (*)(void*)>(s("__cajeta_prof_trace_packets"));
    x.interned = reinterpret_cast<int32_t (*)(void*)>(s("__cajeta_prof_trace_interned"));
    return x;
}
} // namespace

// A repeated name is emitted ONCE (spec §7.4). Asserted on the packet count,
// not just the returned iid: an interning table that hands back a stable iid
// while re-emitting the name every time satisfies the weaker check and defeats
// the entire purpose.
TEST(ProfilerTraceWire, repeatedNameIsEmittedOnce) {
    Writer w = wr();
    ASSERT_NE(w.open, nullptr) << "__cajeta_prof_trace_open unresolved";
    std::vector<uint8_t> mem(static_cast<size_t>(w.wsize()), 0);
    std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                     + "/cajeta-intern-test.pftrace";
    ASSERT_EQ(w.open(mem.data(), path.c_str()), 1);

    uint64_t a = w.intern(mem.data(), "test.App.run");
    int64_t afterFirst = w.packets(mem.data());
    uint64_t b = w.intern(mem.data(), "test.App.run");
    int64_t afterSecond = w.packets(mem.data());

    EXPECT_EQ(a, 1u) << "iids are 1-based; 0 means not-interned";
    EXPECT_EQ(b, a) << "same name must return the same iid";
    EXPECT_EQ(afterSecond, afterFirst) << "repeated name re-emitted a packet";
    EXPECT_EQ(w.interned(mem.data()), 1);

    uint64_t c = w.intern(mem.data(), "test.App.other");
    EXPECT_NE(c, a);
    EXPECT_EQ(w.packets(mem.data()), afterFirst + 1) << "a new name must emit";
    w.close(mem.data());
    std::remove(path.c_str());
}

// Equal strings at DIFFERENT addresses must intern to one iid. Codegen emits a
// frame descriptor per method, so two methods with the same name in different
// modules carry equal strings at different addresses — pointer identity would
// silently emit the name repeatedly and defeat the interning while every other
// assertion here still passed.
TEST(ProfilerTraceWire, internComparesContentNotAddress) {
    Writer w = wr();
    ASSERT_NE(w.open, nullptr);
    std::vector<uint8_t> mem(static_cast<size_t>(w.wsize()), 0);
    std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                     + "/cajeta-intern-addr.pftrace";
    ASSERT_EQ(w.open(mem.data(), path.c_str()), 1);

    std::string s1 = "cajeta.lang.String.substring";
    std::string s2 = "cajeta.lang.String.substring";   // equal, distinct storage
    ASSERT_NE(s1.c_str(), s2.c_str());

    uint64_t a = w.intern(mem.data(), s1.c_str());
    uint64_t b = w.intern(mem.data(), s2.c_str());
    EXPECT_EQ(a, b) << "interning compared addresses, not content";
    EXPECT_EQ(w.interned(mem.data()), 1);
    w.close(mem.data());
    std::remove(path.c_str());
}

// The table must OWN its names. A caller that builds a name into a scratch
// buffer — which Unit 6's transform does for every slice — otherwise leaves the
// table holding a dangling pointer, and the next call's reused buffer compares
// EQUAL to it. The failure is silent and total: every frame resolves to the
// first iid and the trace carries one distinct name.
//
// Regression for CI run 32491747115.
TEST(ProfilerTraceWire, internCopiesOutOfCallerScratch) {
    Writer w = wr();
    ASSERT_NE(w.open, nullptr);
    std::vector<uint8_t> mem(static_cast<size_t>(w.wsize()), 0);
    std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                     + "/cajeta-intern-scratch.pftrace";
    ASSERT_EQ(w.open(mem.data(), path.c_str()), 1);

    char scratch[64];
    std::snprintf(scratch, sizeof(scratch), "test.App.run");
    uint64_t a = w.intern(mem.data(), scratch);
    std::snprintf(scratch, sizeof(scratch), "test.App.middle");   // same buffer
    uint64_t b = w.intern(mem.data(), scratch);
    std::snprintf(scratch, sizeof(scratch), "test.App.inner");
    uint64_t c = w.intern(mem.data(), scratch);

    EXPECT_NE(a, b) << "second name collided with the first via the reused buffer";
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
    EXPECT_EQ(w.interned(mem.data()), 3) << "distinct names did not all intern";

    // And the first name still matches itself after the buffer moved on.
    char again[64];
    std::snprintf(again, sizeof(again), "test.App.run");
    EXPECT_EQ(w.intern(mem.data(), again), a);
    EXPECT_EQ(w.interned(mem.data()), 3);
    w.close(mem.data());
    std::remove(path.c_str());
}

// ── 6.1.d: source locations ───────────────────────────────────────────────
// Locations intern in their OWN iid space, separate from event names. That
// separation is the whole design: a method sampled at forty lines must stay ONE
// EventName (spec §7.4) with forty SourceLocations beside it. Folding
// "file:line" into the slice name would have minted a fresh interned name per
// line and quietly destroyed the interning table's purpose.
TEST(ProfilerTraceWire, sourceLocationsInternSeparatelyFromNames) {
    Writer w = wr();
    auto& base = wire();
    auto internSrc = reinterpret_cast<uint64_t (*)(void*, const char*, const char*, int32_t)>(
        base.jit->lookupRawSymbol("__cajeta_prof_intern_source"));
    auto srcCount = reinterpret_cast<int32_t (*)(void*)>(
        base.jit->lookupRawSymbol("__cajeta_prof_trace_source_count"));
    ASSERT_NE(internSrc, nullptr) << "__cajeta_prof_intern_source unresolved";
    ASSERT_NE(srcCount, nullptr);

    std::vector<uint8_t> mem(static_cast<size_t>(w.wsize()), 0);
    std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp")
                     + "/cajeta-src.pftrace";
    ASSERT_EQ(w.open(mem.data(), path.c_str()), 1);

    // One method, three lines — the shape a sampler actually produces.
    uint64_t l10 = internSrc(mem.data(), "App.cajeta", "test.App.run", 10);
    uint64_t l11 = internSrc(mem.data(), "App.cajeta", "test.App.run", 11);
    uint64_t l10b = internSrc(mem.data(), "App.cajeta", "test.App.run", 10);
    EXPECT_NE(l10, l11) << "different lines must be different source locations";
    EXPECT_EQ(l10, l10b) << "the same triple must dedup";
    EXPECT_EQ(srcCount(mem.data()), 2);

    // The name table is untouched by any of that — the separation under test.
    EXPECT_EQ(w.interned(mem.data()), 0)
        << "interning a source location leaked into the event-name table";
    uint64_t n = w.intern(mem.data(), "test.App.run");
    EXPECT_EQ(n, 1u) << "name iids are their own space, starting at 1";
    EXPECT_EQ(w.interned(mem.data()), 1);
    EXPECT_EQ(srcCount(mem.data()), 2) << "interning a name disturbed the locations";

    w.close(mem.data());
    std::remove(path.c_str());
}
