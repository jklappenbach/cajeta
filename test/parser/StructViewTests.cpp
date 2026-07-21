//
// Session 4 — struct view construction + field access.
//
// A `struct` declares an inline POD layout (packed, host endian in v1).
// `MyStruct(byte[] bytes)` constructs a typed view over the buffer:
// the call bounds-checks (data.size() >= sizeof(struct)) and returns a
// pointer to the buffer's data region. Subsequent field accesses GEP off
// that pointer using the struct's declared field offsets.
//
// What's tested here:
//   - View construction returns a usable pointer
//   - Field reads decode the bytes at the declared offsets
//   - Field writes update the bytes through the view
//
// Out of scope (Session 5+): variable-size fields (String/array inline),
// @BigEndian / @LittleEndian intrinsic bswaps, @Align(natural) padding,
// undersize-buffer bounds-check observation (the runtime aborts; we don't
// have an exception-style failure mode for this yet).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string structSource(const std::string& body) {
    return "package test;\n"
           "@HostEndian\n"
           "public view Header {\n"
           "    int32 version;\n"
           "    int64 timestamp;\n"
           "    int32 payloadLen;\n"
           "}\n"
           "public final class S {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(structSource(body), "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// --- View construction smoke test -------------------------------------------

TEST(StructViewTests, structDeclarationCompiles) {
    // The struct itself just declares — no view construction yet. Verifies
    // the visitor + CajetaStruct prototype generation don't blow up.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Header {\n"
        "    int32 version;\n"
        "    int64 timestamp;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.S"));
}

TEST(StructViewTests, viewConstructionSucceedsOnSufficientBuffer) {
    // 16 bytes is enough for { i32, i64, i32 } = 16 bytes packed.
    EXPECT_EQ(runI32(
        "int32[] bytes = heap int32[4];\n"
        "Header h = Header(bytes);\n"
        "return 0;"), 0);
}

// --- Field write + read round-trip -----------------------------------------

TEST(StructViewTests, writeAndReadInt32FieldThroughView) {
    EXPECT_EQ(runI32(
        "int32[] bytes = heap int32[4];\n"
        "Header h = Header(bytes);\n"
        "h.version = 42;\n"
        "return h.version;"), 42);
}

TEST(StructViewTests, multipleFieldsIndependent) {
    EXPECT_EQ(runI32(
        "int32[] bytes = heap int32[4];\n"
        "Header h = Header(bytes);\n"
        "h.version = 100;\n"
        "h.payloadLen = 25;\n"
        "return h.version + h.payloadLen;"), 125);
}

// --- Field reads start zeroed -----------------------------------------------

TEST(StructViewTests, freshBufferReadsZero) {
    EXPECT_EQ(runI32(
        "int32[] bytes = heap int32[4];\n"
        "Header h = Header(bytes);\n"
        "return h.version;"), 0);   // calloc zeros the buffer
}

// --- Struct passed as parameter is pass-by-pointer --------------------------
//
// Structs are zero-copy views over wire-format buffers (docs/specification/lang/Views.md).
// Method::generatePrototype passes them by pointer, not by value —
// otherwise every call-boundary memcpy would defeat the whole point of
// the view. These probes confirm:
//   - The receiver reads the same field values the caller set.
//   - Mutations through the receiver are visible to the caller after
//     return (aliasing, not copying).

TEST(StructViewTests, structParamReadsCallerValues) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Header {\n"
        "    int32 version;\n"
        "    int64 timestamp;\n"
        "    int32 payloadLen;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 readVersion(Header h) {\n"
        "        return h.version;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[4];\n"
        "        Header h = Header(bytes);\n"
        "        h.version = 99;\n"
        "        return S.readVersion(h);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

TEST(StructViewTests, structParamMutationsAreVisibleToCaller) {
    // The callee writes through the param; the caller reads back and
    // sees the new value. By-value semantics would lose the write
    // when the callee returned. By-pointer (the new default) makes
    // the callee write directly to the caller's buffer.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Header {\n"
        "    int32 version;\n"
        "    int64 timestamp;\n"
        "    int32 payloadLen;\n"
        "}\n"
        "public final class S {\n"
        "    public static void bump(Header h) {\n"
        "        h.version = h.version + 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[4];\n"
        "        Header h = Header(bytes);\n"
        "        h.version = 10;\n"
        "        S.bump(h);\n"
        "        S.bump(h);\n"
        "        S.bump(h);\n"
        "        return h.version;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 13);
}

// --- Struct view-aliasing escape check --------------------------------------
//
// `Header h = Header(bytes); return h;` — when `bytes` is a function-
// scope local, the buffer drops as the function returns, leaving the
// caller with a view of freed memory. The compiler should reject this
// at compile time with CAJETA_ERROR_VIEW_ESCAPE rather than letting it
// silently produce a use-after-free.
//
// The check fires only when the view's source is a function-scope
// LOCAL. View over a PARAMETER (caller-owned buffer) is fine because
// the caller's buffer outlives the call. Views over fields would also
// be fine, but field-as-source isn't probed here since the LocalVar
// detector only matches IdentifierExpression args (the bare-name case
// covers locals AND parameters via the scope lookup).

TEST(StructViewTests, returningViewOfLocalBufferIsRejected) {
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Header {\n"
        "    int32 version;\n"
        "    int64 timestamp;\n"
        "    int32 payloadLen;\n"
        "}\n"
        "public final class S {\n"
        "    public static Header makeView() {\n"
        "        int32[] bytes = heap int32[4];\n"
        "        Header h = Header(bytes);\n"
        "        return h;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_ANY_THROW(CajetaJit::compile(src, "test.S"));
}

TEST(StructViewTests, returningViewOfParamBufferIsOK) {
    // `bytes` is a parameter — caller owns the buffer, so the view
    // is safe to return. View-escape check should NOT fire.
    auto src =
        "package test;\n"
        "@HostEndian\n"
        "public view Header {\n"
        "    int32 version;\n"
        "    int64 timestamp;\n"
        "    int32 payloadLen;\n"
        "}\n"
        "public final class S {\n"
        "    public static Header asHeader(int32[] bytes) {\n"
        "        Header h = Header(bytes);\n"
        "        return h;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = heap int32[4];\n"
        "        Header h = S.asHeader(bytes);\n"
        "        h.version = 77;\n"
        "        return h.version;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 77);
}
