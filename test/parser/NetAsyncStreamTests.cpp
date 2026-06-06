// Tests for the cajeta.net async byte-stream layer (NET-3.5):
//   - AsyncReader : buffered reader over a TcpStream (read / readExact /
//                   readUntil + the AsyncIterator<int8[]> chunk surface)
//   - AsyncWriter : buffered writer over a TcpStream (writeAll / writeByte /
//                   writeString + explicit flush coalescing)
//
// Golden-vector style over CajetaJit, mirroring NetBufferTests.cpp: each
// test compiles a small inline program that exercises the structure and
// returns an int32 the harness asserts on.
//
// Why this is testable without a live socket / reactor:
//   The buffer-management *logic* is pure over the staging ring + the sticky
//   EOF flag. AsyncReader exposes a test/pre-staging seam (`stage` + `markEof`)
//   that pushes bytes into the ring and asserts peer-close WITHOUT a socket
//   read — every drain path (read / readExact / readUntil / next) then serves
//   from the ring and, on an empty ring with EOF set, short-circuits the
//   socket refill (fillFromSocket returns 0 immediately once eof is set). So
//   the readiness-loop primitives the async ops are built from (readAsync /
//   writeAllAsync, pinned natively in NetAsyncOpsTests) are never reached here.
//   The wrapped TcpStream is a closed-fd sentinel (`heap TcpStream(-1)`): its
//   ctor + the inert stub close() compile and run in the JIT today (same as
//   NetConnectFallbackTests constructs one); no NET-1.3 intrinsic dispatch is
//   needed. AsyncWriter tests size the buffer so no auto-flush fires, so
//   writeAllAsync (which would need the reactor) is never invoked — the
//   coalescing logic is asserted via `pending()` alone.
//
// The end-to-end fiber-parking reads/writes over a real loopback socket are
// the JIT ReactorTests.* the plan names; they need the (not-yet-wired)
// compiler net-receiver dispatch + a live scheduler, like NetAsyncOpsTests'
// header documents.
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

// Wrap a method body in a class importing the async-stream surface. The
// body must `return` an int32. A fresh closed-fd TcpStream sentinel is the
// borrowed transport every test wraps.
std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.String;\n"
           "import cajeta.lang.Optional;\n"
           "import cajeta.net.TcpStream;\n"
           "import cajeta.net.AsyncReader;\n"
           "import cajeta.net.AsyncWriter;\n"
           "public final class S {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}
} // namespace

// ---------------------------------------------------------------------------
// AsyncReader — buffered read over the staging ring
// ---------------------------------------------------------------------------

TEST(AsyncReaderTests, freshReaderHasNothingBufferedAndIsNotAtEnd) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 64);\n"
        "int32 ended = 0;\n"
        "if (r.atEnd()) { ended = 1; }\n"
        "return r.buffered() * 10 + ended;\n")), 0);
}

TEST(AsyncReaderTests, stagedBytesAreBufferedAndReadBack) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 32);\n"
        "int8[] src = new int8[4];\n"
        "src[0]=(int8)10; src[1]=(int8)20; src[2]=(int8)30; src[3]=(int8)40;\n"
        "int32 staged = r.stage(src, 4);\n"
        "int8[] dst = new int8[4];\n"
        "int32 n = r.read(dst, 0, 4);\n"
        "int32 sum = ((int32)dst[0])+((int32)dst[1])+((int32)dst[2])+((int32)dst[3]);\n"
        // staged=4, n=4, sum=100 -> 4*100000 + 4*10000 + 100 = 440100
        "return staged * 100000 + n * 10000 + sum;\n")), 440100);
}

TEST(AsyncReaderTests, readDrainedThenEofReturnsZero) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 16);\n"
        "int8[] src = new int8[2];\n"
        "src[0]=(int8)1; src[1]=(int8)2;\n"
        "r.stage(src, 2);\n"
        "r.markEof();\n"                       // peer closed; ring still holds 2
        "int8[] dst = new int8[8];\n"
        "int32 n1 = r.read(dst, 0, 8);\n"      // drains 2 from the ring
        "int32 n2 = r.read(dst, 0, 8);\n"      // ring empty + eof -> 0, no socket
        "int32 ended = 0;\n"
        "if (r.atEnd()) { ended = 1; }\n"
        // n1=2, n2=0, ended=1 -> 2*100 + 0*10 + 1 = 201
        "return n1 * 100 + n2 * 10 + ended;\n")), 201);
}

TEST(AsyncReaderTests, readExactFillsAcrossTheStagedBytes) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 32);\n"
        "int8[] src = new int8[5];\n"
        "int32 i = 0;\n"
        "while (i < 5) { src[i] = (int8)(i + 1); i = i + 1; }\n"
        "r.stage(src, 5);\n"
        "int8[] dst = new int8[5];\n"
        "r.readExact(dst, 0, 5);\n"
        "int32 sum = 0;\n"
        "int32 j = 0;\n"
        "while (j < 5) { sum = sum + ((int32) dst[j]); j = j + 1; }\n"
        // 1+2+3+4+5 = 15
        "return sum;\n")), 15);
}

TEST(AsyncReaderTests, readExactPastEofRaises) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 16);\n"
        "int8[] src = new int8[3];\n"
        "src[0]=(int8)7; src[1]=(int8)8; src[2]=(int8)9;\n"
        "r.stage(src, 3);\n"
        "r.markEof();\n"                       // only 3 will ever arrive
        "int8[] dst = new int8[5];\n"
        "try {\n"
        "    r.readExact(dst, 0, 5);\n"        // wants 5, EOF after 3 -> raise
        "    return -1;\n"
        "} catch (cajeta.net.NetException e) {\n"
        "    return 1;\n"
        "}\n")), 1);
}

TEST(AsyncReaderTests, readUntilIncludesDelimiter) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 64);\n"
        // "ab\nXY" — read up to and including the first LF (10).
        "int8[] src = new int8[5];\n"
        "src[0]=(int8)97; src[1]=(int8)98; src[2]=(int8)10;\n"   // a b \n
        "src[3]=(int8)88; src[4]=(int8)89;\n"                    // X Y
        "r.stage(src, 5);\n"
        "int8[] line = r.readUntil((int8) 10, 0);\n"
        "int32 len = line.count();\n"          // 3 : "ab\n"
        "int32 last = (int32) line[len - 1];\n"// 10 (LF, included)
        "int32 left = r.buffered();\n"         // 2 left: "XY"
        // len=3, last=10, left=2 -> 3*1000 + 10*10 + 2 = 3102
        "return len * 1000 + last * 10 + left;\n")), 3102);
}

TEST(AsyncReaderTests, readUntilHonorsMaxBytesCap) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 64);\n"
        // No delimiter present; cap at 4 returns the first 4 bytes.
        "int8[] src = new int8[6];\n"
        "int32 i = 0;\n"
        "while (i < 6) { src[i] = (int8) 65; i = i + 1; }\n"   // "AAAAAA"
        "r.stage(src, 6);\n"
        "int8[] got = r.readUntil((int8) 10, 4);\n"   // LF never seen, cap 4
        "int32 len = got.count();\n"                  // 4
        "int32 left = r.buffered();\n"                // 2 still staged
        // len=4, left=2 -> 4*100 + 2 = 402
        "return len * 100 + left;\n")), 402);
}

TEST(AsyncReaderTests, readUntilAtEofReturnsRemainderWithoutDelimiter) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 32);\n"
        "int8[] src = new int8[3];\n"
        "src[0]=(int8)1; src[1]=(int8)2; src[2]=(int8)3;\n"   // no LF
        "r.stage(src, 3);\n"
        "r.markEof();\n"
        "int8[] got = r.readUntil((int8) 10, 0);\n"   // EOF before delimiter
        "int32 len = got.count();\n"                  // 3
        "int32 ended = 0;\n"
        "if (r.atEnd()) { ended = 1; }\n"
        // len=3, ended=1 -> 3*10 + 1 = 31
        "return len * 10 + ended;\n")), 31);
}

TEST(AsyncReaderTests, nextYieldsChunkThenTerminatesAtEof) {
    EXPECT_EQ(runI32(makeSource(
        // chunk size 4: a 6-byte stage yields a 4-byte chunk then a 2-byte
        // chunk, then an empty Optional once drained + EOF.
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncReader r = heap AsyncReader(t, 4);\n"
        "int8[] src = new int8[6];\n"
        "int32 i = 0;\n"
        "while (i < 6) { src[i] = (int8) 1; i = i + 1; }\n"
        "int32 staged = r.stage(src, 6);\n"   // ring cap 4 -> only 4 fit
        "r.markEof();\n"
        "Optional<int8[]> c1 = r.next();\n"   // present, 4 bytes
        "int32 a = 0;\n"
        "if (c1.isPresent()) { a = c1.get().count(); }\n"  // 4
        "Optional<int8[]> c2 = r.next();\n"   // ring empty + eof -> empty
        "int32 term = 0;\n"
        "if (c2.isEmpty()) { term = 1; }\n"
        // staged=4 (cap-bounded), a=4, term=1 -> 4*1000 + 4*100 + 1 = 4401
        "return staged * 1000 + a * 100 + term;\n")), 4401);
}

// ---------------------------------------------------------------------------
// AsyncWriter — coalescing buffer (no flush => no socket)
// ---------------------------------------------------------------------------

TEST(AsyncWriterTests, freshWriterHasNothingPending) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncWriter w = heap AsyncWriter(t, 64);\n"
        "return w.pending();\n")), 0);
}

TEST(AsyncWriterTests, writeAllStagesWithoutSending) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncWriter w = heap AsyncWriter(t, 64);\n"  // big enough: no auto-flush
        "int8[] data = new int8[5];\n"
        "int32 i = 0;\n"
        "while (i < 5) { data[i] = (int8) 7; i = i + 1; }\n"
        "w.writeAll(data, 0, 5);\n"
        "return w.pending();\n")), 5);   // all 5 staged, none flushed
}

TEST(AsyncWriterTests, writeByteAccumulates) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncWriter w = heap AsyncWriter(t, 16);\n"
        "w.writeByte((int8) 13);\n"   // CR
        "w.writeByte((int8) 10);\n"   // LF
        "w.writeByte((int8) 65);\n"   // A
        "return w.pending();\n")), 3);
}

TEST(AsyncWriterTests, writeStringStagesItsBytes) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncWriter w = heap AsyncWriter(t, 64);\n"
        "String s = \"GET / HTTP/1.1\";\n"   // 14 ASCII bytes
        "w.writeString(s);\n"
        "return w.pending();\n")), 14);
}

TEST(AsyncWriterTests, writeAllRespectsOffsetAndLength) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncWriter w = heap AsyncWriter(t, 64);\n"
        "int8[] data = new int8[10];\n"
        "int32 i = 0;\n"
        "while (i < 10) { data[i] = (int8) i; i = i + 1; }\n"
        "w.writeAll(data, 3, 4);\n"   // bytes [3,4,5,6] -> 4 staged
        "return w.pending();\n")), 4);
}

TEST(AsyncWriterTests, mixedWritesCoalesceIntoOneBuffer) {
    EXPECT_EQ(runI32(makeSource(
        "TcpStream t = heap TcpStream(-1);\n"
        "AsyncWriter w = heap AsyncWriter(t, 128);\n"
        "String head = \"HTTP/1.1 200 OK\";\n"  // 15 bytes
        "w.writeString(head);\n"
        "w.writeByte((int8) 13);\n"             // +1
        "w.writeByte((int8) 10);\n"             // +1
        "int8[] body = new int8[8];\n"
        "int32 i = 0;\n"
        "while (i < 8) { body[i] = (int8) 88; i = i + 1; }\n"
        "w.writeAll(body, 0, 8);\n"             // +8
        // 15 + 1 + 1 + 8 = 25, all coalesced, nothing flushed
        "return w.pending();\n")), 25);
}
