// Tests for the cajeta.io.net buffer-management layer (NET-11.5):
//   - ByteBuffer  : read/write-cursor byte buffer (the pooled unit)
//   - RingBuffer  : circular buffer + high/low watermark backpressure
//   - BufferPool  : pooled ByteBuffer allocator, reuse-bounded
//
// Golden-vector style over CajetaJit: each test compiles a small
// inline program that exercises the structure and returns an int32
// the harness asserts on. Pure logic — no sockets, no native bridge.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// ---------------------------------------------------------------------------
// ByteBuffer
// ---------------------------------------------------------------------------

TEST(ByteBufferTests, freshBufferIsEmptyWithFullWritableRoom) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ByteBuffer b = heap ByteBuffer(64);\n"
        "        return b.readable() * 1000 + b.writable();\n"
        "    }\n"
        "}\n";
    // readable() == 0, writable() == 64
    EXPECT_EQ(runI32(src), 64);
}

TEST(ByteBufferTests, writeThenReadRoundTrips) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ByteBuffer b = heap ByteBuffer(16);\n"
        "        int8[] src = heap int8[4];\n"
        "        src[0] = (int8) 10; src[1] = (int8) 20;\n"
        "        src[2] = (int8) 30; src[3] = (int8) 40;\n"
        "        int32 w = b.write(src, 0, 4);\n"
        "        int8[] dst = heap int8[4];\n"
        "        int32 r = b.read(dst, 0, 4);\n"
        "        int32 sum = ((int32) dst[0]) + ((int32) dst[1])\n"
        "                  + ((int32) dst[2]) + ((int32) dst[3]);\n"
        "        return w * 100000 + r * 10000 + sum;\n"
        "    }\n"
        "}\n";
    // w=4, r=4, sum=100 -> 4*100000 + 4*10000 + 100 = 440100
    EXPECT_EQ(runI32(src), 440100);
}

TEST(ByteBufferTests, writeStopsAtCapacityShortWrite) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ByteBuffer b = heap ByteBuffer(3);\n"
        "        int8[] src = heap int8[5];\n"
        "        int32 i = 0;\n"
        "        while (i < 5) { src[i] = (int8) 1; i = i + 1; }\n"
        "        int32 w = b.write(src, 0, 5);\n"
        "        return w * 100 + b.writable();\n"
        "    }\n"
        "}\n";
    // only 3 fit -> w=3, writable now 0 -> 300
    EXPECT_EQ(runI32(src), 300);
}

TEST(ByteBufferTests, advanceReadAdvanceWriteTrackCursors) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ByteBuffer b = heap ByteBuffer(32);\n"
        "        b.advanceWrite(10);\n"   // simulate a 10-byte socket read
        "        b.advanceRead(4);\n"     // parser consumed 4
        "        return b.readable() * 100 + b.writable();\n"
        "    }\n"
        "}\n";
    // readable = 10-4 = 6, writable = 32-10 = 22 -> 622
    EXPECT_EQ(runI32(src), 622);
}

TEST(ByteBufferTests, advanceClampsToBounds) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ByteBuffer b = heap ByteBuffer(8);\n"
        "        b.advanceWrite(100);\n"  // clamps to 8
        "        b.advanceRead(100);\n"   // clamps to 8 readable
        "        return b.writePosition() * 100 + b.readPosition();\n"
        "    }\n"
        "}\n";
    // writePos clamped to 8, readPos clamped to 8 -> 808
    EXPECT_EQ(runI32(src), 808);
}

TEST(ByteBufferTests, compactSlidesUnreadBytesToFront) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ByteBuffer b = heap ByteBuffer(8);\n"
        "        int8[] src = heap int8[6];\n"
        "        src[0]=(int8)1; src[1]=(int8)2; src[2]=(int8)3;\n"
        "        src[3]=(int8)4; src[4]=(int8)5; src[5]=(int8)6;\n"
        "        b.write(src, 0, 6);\n"
        "        b.advanceRead(4);\n"        // consumed 1..4, 5,6 remain
        "        b.compact();\n"
        "        int32 rp = b.readPosition();\n"   // 0
        "        int32 wp = b.writePosition();\n"  // 2
        "        int32 first = (int32) b.at(0);\n"   // moved byte 5
        "        int32 second = (int32) b.at(1);\n"  // moved byte 6
        "        return rp * 100000 + wp * 10000 + first * 100 + second;\n"
        "    }\n"
        "}\n";
    // rp=0, wp=2, first=5, second=6 -> 0 + 20000 + 500 + 6 = 20506
    EXPECT_EQ(runI32(src), 20506);
}

TEST(ByteBufferTests, clearResetsCursorsKeepsCapacity) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ByteBuffer b = heap ByteBuffer(16);\n"
        "        b.advanceWrite(10);\n"
        "        b.clear();\n"
        "        return b.readable() * 1000 + b.writable() * 10 + b.capacity();\n"
        "    }\n"
        "}\n";
    // readable=0, writable=16, capacity=16 -> 0 + 160 + 16 = 176
    EXPECT_EQ(runI32(src), 176);
}

TEST(ByteBufferTests, reserveGrowsBeyondCapacityPreservingContent) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        ByteBuffer b = heap ByteBuffer(4);\n"
        "        int8[] src = heap int8[4];\n"
        "        src[0]=(int8)7; src[1]=(int8)8; src[2]=(int8)9; src[3]=(int8)11;\n"
        "        b.write(src, 0, 4);\n"      // full
        "        b.reserve(8);\n"            // must grow: writable was 0
        "        int32 cap = b.capacity();\n"
        "        int32 keep0 = (int32) b.at(0);\n"   // content preserved
        "        int32 keep3 = (int32) b.at(3);\n"
        "        int32 wok = b.writable();\n" // >= 8
        "        int32 grew = 0;\n"
        "        if (cap >= 12) { grew = 1; }\n"  // 4 written + 8 reserve
        "        int32 wokFlag = 0;\n"
        "        if (wok >= 8) { wokFlag = 1; }\n"
        "        return grew * 1000 + wokFlag * 100 + keep0 + keep3;\n"
        "    }\n"
        "}\n";
    // grew=1, wokFlag=1, keep0=7, keep3=11 -> 1000 + 100 + 18 = 1118
    EXPECT_EQ(runI32(src), 1118);
}

// ---------------------------------------------------------------------------
// RingBuffer
// ---------------------------------------------------------------------------

TEST(RingBufferTests, writeReadWrapsAround) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.RingBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        RingBuffer r = heap RingBuffer(4, 1, 3);\n"
        "        int8[] a = heap int8[3];\n"
        "        a[0]=(int8)1; a[1]=(int8)2; a[2]=(int8)3;\n"
        "        r.write(a, 0, 3);\n"        // size 3, tail at 3
        "        int8[] out = heap int8[2];\n"
        "        r.read(out, 0, 2);\n"       // drains 1,2 ; head at 2, size 1
        "        int8[] b = heap int8[3];\n"
        "        b[0]=(int8)4; b[1]=(int8)5; b[2]=(int8)6;\n"
        "        int32 w = r.write(b, 0, 3);\n"  // free=3 -> all fit, wraps
        "        return w * 100 + r.size();\n"
        "    }\n"
        "}\n";
    // w=3, size = 1 + 3 = 4 -> 304
    EXPECT_EQ(runI32(src), 304);
}

TEST(RingBufferTests, writeShortWhenFull) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.RingBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        RingBuffer r = heap RingBuffer(4, 1, 4);\n"
        "        int8[] a = heap int8[10];\n"
        "        int32 i = 0;\n"
        "        while (i < 10) { a[i] = (int8) 1; i = i + 1; }\n"
        "        int32 w = r.write(a, 0, 10);\n"  // only 4 fit
        "        int32 full = 0;\n"
        "        if (r.isFull()) { full = 1; }\n"
        "        return w * 10 + full;\n"
        "    }\n"
        "}\n";
    // w=4, full=1 -> 41
    EXPECT_EQ(runI32(src), 41);
}

TEST(RingBufferTests, highWatermarkTrips) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.RingBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        RingBuffer r = heap RingBuffer(10, 2, 7);\n"
        "        int8[] a = heap int8[6];\n"
        "        int32 i = 0;\n"
        "        while (i < 6) { a[i] = (int8) 1; i = i + 1; }\n"
        "        r.write(a, 0, 6);\n"           // size 6 < high 7
        "        int32 before = 0;\n"
        "        if (r.isOverHighWatermark()) { before = 1; }\n"
        "        int8[] one = heap int8[1];\n"
        "        one[0] = (int8) 9;\n"
        "        r.write(one, 0, 1);\n"          // size 7 == high
        "        int32 after = 0;\n"
        "        if (r.isOverHighWatermark()) { after = 1; }\n"
        "        return before * 10 + after;\n"
        "    }\n"
        "}\n";
    // before=0 (6<7), after=1 (7>=7) -> 1
    EXPECT_EQ(runI32(src), 1);
}

TEST(RingBufferTests, lowWatermarkResumesAfterDrain) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.RingBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        RingBuffer r = heap RingBuffer(10, 2, 7);\n"
        "        int8[] a = heap int8[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) { a[i] = (int8) 1; i = i + 1; }\n"
        "        r.write(a, 0, 8);\n"            // size 8, over high
        "        int32 highNow = 0;\n"
        "        if (r.isUnderLowWatermark()) { highNow = 1; }\n"  // 8>2 -> 0
        "        int8[] out = heap int8[6];\n"
        "        r.read(out, 0, 6);\n"           // size 2 == low
        "        int32 lowNow = 0;\n"
        "        if (r.isUnderLowWatermark()) { lowNow = 1; }\n"   // 2<=2 -> 1
        "        return highNow * 10 + lowNow;\n"
        "    }\n"
        "}\n";
    // highNow=0, lowNow=1 -> 1
    EXPECT_EQ(runI32(src), 1);
}

TEST(RingBufferTests, peekDoesNotConsume) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.RingBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        RingBuffer r = heap RingBuffer(8, 1, 6);\n"
        "        int8[] a = heap int8[4];\n"
        "        a[0]=(int8)11; a[1]=(int8)22; a[2]=(int8)33; a[3]=(int8)44;\n"
        "        r.write(a, 0, 4);\n"
        "        int32 p0 = (int32) r.peek(0);\n"
        "        int32 p2 = (int32) r.peek(2);\n"
        "        int32 sz = r.size();\n"   // unchanged: still 4
        "        return p0 * 10000 + p2 * 100 + sz;\n"
        "    }\n"
        "}\n";
    // p0=11, p2=33, sz=4 -> 110000 + 3300 + 4 = 113304
    EXPECT_EQ(runI32(src), 113304);
}

TEST(RingBufferTests, skipConsumesWithoutCopy) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.RingBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        RingBuffer r = heap RingBuffer(8, 1, 6);\n"
        "        int8[] a = heap int8[5];\n"
        "        a[0]=(int8)1; a[1]=(int8)2; a[2]=(int8)3; a[3]=(int8)4; a[4]=(int8)5;\n"
        "        r.write(a, 0, 5);\n"
        "        int32 sk = r.skip(3);\n"     // drop 1,2,3
        "        int32 nextByte = (int32) r.peek(0);\n" // now 4
        "        return sk * 100 + nextByte * 10 + r.size();\n"
        "    }\n"
        "}\n";
    // sk=3, nextByte=4, size=2 -> 300 + 40 + 2 = 342
    EXPECT_EQ(runI32(src), 342);
}

TEST(RingBufferTests, watermarksClampToSaneBand) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.RingBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        // high > capacity clamps to capacity; low > high clamps to high
        "        RingBuffer r = heap RingBuffer(5, 99, 99);\n"
        "        return r.highWatermarkLevel() * 100 + r.lowWatermarkLevel();\n"
        "    }\n"
        "}\n";
    // high clamps to 5, low clamps to high(5) -> 505
    EXPECT_EQ(runI32(src), 505);
}

// ---------------------------------------------------------------------------
// BufferPool
// ---------------------------------------------------------------------------

TEST(BufferPoolTests, acquireGivesSlabSizedBuffer) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.BufferPool;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        BufferPool p = heap BufferPool(128, 4);\n"
        "        ByteBuffer b #= p.acquire();\n"
        "        return b.capacity();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 128);
}

TEST(BufferPoolTests, releaseRetainsIdleBuffer) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.BufferPool;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        BufferPool p = heap BufferPool(64, 4);\n"
        "        ByteBuffer b #= p.acquire();\n"
        "        p.release(#b);\n"
        "        return p.idle();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// The acceptance test named in the plan:
//   "Buffer pool reuse: N sequential connections allocate <= pool cap
//    buffers (no unbounded growth)."  -> BufferPoolTests.reuseStaysBounded
TEST(BufferPoolTests, reuseStaysBounded) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.BufferPool;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        BufferPool p = heap BufferPool(256, 4);\n"
        "        int32 i = 0;\n"
        // 100 sequential connections: acquire then release each.
        "        while (i < 100) {\n"
        "            ByteBuffer b #= p.acquire();\n"
        "            b.advanceWrite(10);\n"   // use it like a connection would
        "            p.release(#b);\n"
        "            i = i + 1;\n"
        "        }\n"
        // Only the very first acquire allocated; the rest reused.
        "        return p.allocations();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(BufferPoolTests, concurrentHoldsAllocateUpToNeed) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.BufferPool;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        BufferPool p = heap BufferPool(64, 4);\n"
        // Hold 3 at once -> 3 fresh allocations, none reused yet.
        "        ByteBuffer a #= p.acquire();\n"
        "        ByteBuffer b #= p.acquire();\n"
        "        ByteBuffer c #= p.acquire();\n"
        "        int32 allocAfterHold = p.allocations();\n"  // 3
        "        p.release(#a);\n"
        "        p.release(#b);\n"
        "        p.release(#c);\n"
        "        ByteBuffer d #= p.acquire();\n"  // reuse, no new alloc
        "        return allocAfterHold * 10 + p.allocations();\n"
        "    }\n"
        "}\n";
    // allocAfterHold=3, allocations still 3 after reuse -> 33
    EXPECT_EQ(runI32(src), 33);
}

TEST(BufferPoolTests, releaseBeyondMaxIdleDropsExtras) {
    auto src =
        "package test;\n"
        "import cajeta.io.net.BufferPool;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        BufferPool p = heap BufferPool(64, 2);\n"  // maxIdle 2
        "        ByteBuffer a #= p.acquire();\n"
        "        ByteBuffer b #= p.acquire();\n"
        "        ByteBuffer c #= p.acquire();\n"
        "        p.release(#a);\n"
        "        p.release(#b);\n"
        "        p.release(#c);\n"  // third can't be retained
        "        return p.idle();\n"  // capped at 2
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// ---------------------------------------------------------------------------
// ByteBuffer over a ByteChannel (primavera-web plan 1.0): fill the writable
// tail straight from a channel, send the readable region in one write, and
// borrow the backing store for windows. A memory channel stands in for the
// socket; the counters prove the channel was called exactly when it should.
// ---------------------------------------------------------------------------

namespace {
const char* kMemChannel =
    "final class MemChannel implements ByteChannel {\n"
    "    int8[] src;\n"
    "    int32 srcLen;\n"
    "    int32 cursor;\n"
    "    int8[] sink;\n"
    "    int32 sinkLen;\n"
    "    int32 reads;\n"
    "    int32 writes;\n"
    "    MemChannel(int8[] src, int32 srcLen) {\n"
    "        this.src = heap int8[64];\n"
    "        int32 i = 0;\n"
    "        while (i < srcLen) { this.src[i] = src[i]; i = i + 1; }\n"
    "        this.srcLen = srcLen;\n"
    "        this.cursor = 0;\n"
    "        this.sink = heap int8[64];\n"
    "        this.sinkLen = 0;\n"
    "        this.reads = 0;\n"
    "        this.writes = 0;\n"
    "    }\n"
    "    public int64 readAsync(int8[] buf, int64 offset, int64 length) {\n"
    "        this.reads = this.reads + 1;\n"
    "        int64 n = (int64) (this.srcLen - this.cursor);\n"
    "        if (n > length) { n = length; }\n"
    "        int64 i = 0;\n"
    "        while (i < n) { buf[offset + i] = this.src[this.cursor + (int32) i]; i = i + 1; }\n"
    "        this.cursor = this.cursor + (int32) n;\n"
    "        return n;\n"
    "    }\n"
    "    public void writeAllAsync(int8[] data, int64 offset, int64 length) {\n"
    "        this.writes = this.writes + 1;\n"
    "        int64 i = 0;\n"
    "        while (i < length) { this.sink[this.sinkLen + (int32) i] = data[offset + i]; i = i + 1; }\n"
    "        this.sinkLen = this.sinkLen + (int32) length;\n"
    "    }\n"
    "    public int64 readWithin(int8[] buf, int64 offset, int64 length, int32 timeoutMs) {\n"
    "        return this.readAsync(buf, offset, length);\n"
    "    }\n"
    "    public void close() { return; }\n"
    "}\n";

std::string channelProgram(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.io.net.ByteBuffer;\n"
        "import cajeta.io.net.ByteChannel;\n"
        + std::string(kMemChannel) +
        "public final class S {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
}
} // namespace

TEST(ByteBufferChannelTests, fillAsyncLandsInTheWritableTailAndAdvances) {
    // 3 bytes already readable, then a fill of 5 from the channel: the fill
    // must start at writePos 3, leave the first 3 alone, and advance to 8.
    auto src = channelProgram(
        "        int8[] seed = heap int8[5];\n"
        "        seed[0] = (int8) 1; seed[1] = (int8) 2; seed[2] = (int8) 3; seed[3] = (int8) 4; seed[4] = (int8) 5;\n"
        "        MemChannel ch = heap MemChannel(seed, 5);\n"
        "        ByteBuffer b = heap ByteBuffer(16);\n"
        "        int8[] pre = heap int8[3];\n"
        "        pre[0] = (int8) 7; pre[1] = (int8) 8; pre[2] = (int8) 9;\n"
        "        int32 w = b.write(pre, 0, 3);\n"
        "        int32 n = b.fillAsync(ch);\n"
        "        int32 ok = 0;\n"
        "        if (n == 5) { ok = ok + 1; }\n"
        "        if (b.writePosition() == 8 && b.readPosition() == 0) { ok = ok + 10; }\n"
        "        if (b.at(2) == (int8) 9 && b.at(3) == (int8) 1 && b.at(7) == (int8) 5) { ok = ok + 100; }\n"
        "        if (ch.reads == 1) { ok = ok + 1000; }\n"
        "        return ok;\n");
    EXPECT_EQ(runI32(src), 1111);
}

TEST(ByteBufferChannelTests, fillAsyncIsBoundedByWritableAndSkipsTheChannelWhenFull) {
    auto src = channelProgram(
        "        int8[] seed = heap int8[10];\n"
        "        int32 i = 0;\n"
        "        while (i < 10) { seed[i] = (int8) (i + 1); i = i + 1; }\n"
        "        MemChannel ch = heap MemChannel(seed, 10);\n"
        "        ByteBuffer b = heap ByteBuffer(4);\n"
        "        int32 first = b.fillAsync(ch);\n"
        "        int32 second = b.fillAsync(ch);\n"
        "        int32 ok = 0;\n"
        "        if (first == 4 && b.writable() == 0) { ok = ok + 1; }\n"
        "        if (second == 0 && ch.reads == 1) { ok = ok + 10; }\n"
        "        return ok;\n");
    EXPECT_EQ(runI32(src), 11);
}

TEST(ByteBufferChannelTests, fillAsyncAtEofReturnsZeroAndMovesNothing) {
    auto src = channelProgram(
        "        int8[] none = heap int8[1];\n"
        "        MemChannel ch = heap MemChannel(none, 0);\n"
        "        ByteBuffer b = heap ByteBuffer(8);\n"
        "        int32 n = b.fillAsync(ch);\n"
        "        int32 ok = 0;\n"
        "        if (n == 0 && b.writePosition() == 0 && b.readable() == 0) { ok = 1; }\n"
        "        return ok;\n");
    EXPECT_EQ(runI32(src), 1);
}

TEST(ByteBufferChannelTests, writeToAsyncSendsOnlyTheReadableRegionInOneWrite) {
    // Six bytes written, two consumed: the send must carry bytes 2..5 in a
    // single writeAllAsync and leave the buffer drained.
    auto src = channelProgram(
        "        int8[] none = heap int8[1];\n"
        "        MemChannel ch = heap MemChannel(none, 0);\n"
        "        ByteBuffer b = heap ByteBuffer(16);\n"
        "        int8[] six = heap int8[6];\n"
        "        int32 i = 0;\n"
        "        while (i < 6) { six[i] = (int8) (10 + i); i = i + 1; }\n"
        "        int32 w = b.write(six, 0, 6);\n"
        "        b.advanceRead(2);\n"
        "        b.writeToAsync(ch);\n"
        "        int32 ok = 0;\n"
        "        if (ch.writes == 1 && ch.sinkLen == 4) { ok = ok + 1; }\n"
        "        if (ch.sink[0] == (int8) 12 && ch.sink[3] == (int8) 15) { ok = ok + 10; }\n"
        "        if (b.readable() == 0) { ok = ok + 100; }\n"
        "        return ok;\n");
    EXPECT_EQ(runI32(src), 111);
}

TEST(ByteBufferChannelTests, writeToAsyncWithNothingReadableTouchesNoChannel) {
    auto src = channelProgram(
        "        int8[] none = heap int8[1];\n"
        "        MemChannel ch = heap MemChannel(none, 0);\n"
        "        ByteBuffer b = heap ByteBuffer(8);\n"
        "        b.writeToAsync(ch);\n"
        "        return ch.writes;\n");
    EXPECT_EQ(runI32(src), 0);
}

TEST(ByteBufferChannelTests, arrayIsTheBackingStoreNotACopy) {
    // A window built on array() at readPosition sees the same bytes at() sees,
    // and a write through it is visible to the buffer: shared storage, no copy.
    auto src = channelProgram(
        "        int8[] none = heap int8[1];\n"
        "        MemChannel ch = heap MemChannel(none, 0);\n"
        "        ByteBuffer b = heap ByteBuffer(8);\n"
        "        int8[] two = heap int8[2];\n"
        "        two[0] = (int8) 5; two[1] = (int8) 6;\n"
        "        int32 w = b.write(two, 0, 2);\n"
        "        int8[] store = b.array();\n"
        "        int32 ok = 0;\n"
        "        if (store[1] == (int8) 6 && (int64) store.count() == 8L) { ok = ok + 1; }\n"
        "        store[0] = (int8) 42;\n"
        "        if (b.at(0) == (int8) 42) { ok = ok + 10; }\n"
        "        return ok;\n");
    EXPECT_EQ(runI32(src), 11);
}
