//
// Session 5.5b — variable-size struct fields.
//
// `String` as a struct field lays out as an inline `i32 length` + `length`
// bytes. Reading the field allocates a null-terminated copy so the result is
// compatible with the existing String stdlib. Writing the field is a static
// error (in-place resize isn't possible — the buffer was sized at allocation).
//
// What's tested:
//   - Struct with a single trailing `String` field compiles.
//   - Reading the field produces an owned copy with the correct content.
//   - Reassigning the field is rejected with CAJETA_ERROR_VARSIZE_FIELD_ASSIGN.
//   - A fixed-size field after a variable-size field is rejected at
//     declaration with CAJETA_ERROR_VARSIZE_FIELD_NOT_LAST.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.V");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

void expectErrorId(const std::string& src, const std::string& expectedId) {
    try {
        CajetaJit::compile(src, "test.V");
        FAIL() << "expected " << expectedId << " but compile succeeded";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectedId)
            << "got error '" << e.getErrorId() << "' with message: " << e.getMessage();
    } catch (std::exception& e) {
        FAIL() << "expected cajeta::Exception, got std::exception: " << e.what();
    }
}

} // namespace

// --- Declaration shape -----------------------------------------------------

TEST(VariableSizeStructTests, structWithTrailingStringDeclares) {
    auto src =
        "package test;\n"
        "public struct UserRecord {\n"
        "    int32 id;\n"
        "    String name;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.V"));
}

// --- Read: hand-pack the buffer, then read the String through the view ----

TEST(VariableSizeStructTests, readInlineStringContent) {
    // Buffer layout (packed):
    //   offset 0..4   : id (i32) = 7
    //   offset 4..8   : name.length (i32) = 5
    //   offset 8..13  : name bytes = "alice"
    //
    // We construct the buffer as int32[4] = 16 bytes (more than enough for
    // 8 fixed + 5 bytes of name). The fixed-size header is 8 bytes; the
    // bounds check verifies that at view-construction time.
    auto src =
        "package test;\n"
        "public struct UserRecord {\n"
        "    int32 id;\n"
        "    String name;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[4];\n"
        "        bytes[0] = 7;\n"           // id
        "        bytes[1] = 5;\n"           // name.length
        // Pack "alice" (97 108 105 99 101) into the trailing int32s.
        // little-endian: int32 at offset 8 = 'a'|('l'<<8)|('i'<<16)|('c'<<24)
        //   = 0x63696C61 = 1667853409
        // int32 at offset 12: 'e' = 101 in byte 0, rest zero
        "        bytes[2] = 1667853409;\n"
        "        bytes[3] = 101;\n"
        "        UserRecord u = UserRecord(bytes);\n"
        "        String name = u.name;\n"
        "        if (name.equals(\"alice\")) return 1;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

TEST(VariableSizeStructTests, sizeReportsInlineLength) {
    auto src =
        "package test;\n"
        "public struct UserRecord {\n"
        "    int32 id;\n"
        "    String name;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[4];\n"
        "        bytes[1] = 3;\n"           // length = 3
        // Three non-zero leading bytes (size() uses strlen on the owned copy,
        // so nulls would truncate it). 0x00010203 in LE byte order = bytes
        // {0x03, 0x02, 0x01, 0x00}; the first three are non-null.
        "        bytes[2] = 66051;\n"       // 0x00010203 = 66051
        "        UserRecord u = UserRecord(bytes);\n"
        "        return (int32) u.name.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// --- Mutation rule: variable-size field reassignment is a static error ----

TEST(VariableSizeStructTests, varSizeFieldAssignmentRejected) {
    auto src =
        "package test;\n"
        "public struct UserRecord {\n"
        "    int32 id;\n"
        "    String name;\n"
        "}\n"
        "public final class V {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[4];\n"
        "        UserRecord u = UserRecord(bytes);\n"
        "        u.name = \"alice\";\n"     // static error
        "        return 0;\n"
        "    }\n"
        "}\n";
    expectErrorId(src, "CAJETA_ERROR_VARSIZE_FIELD_ASSIGN");
}

// --- Layout rule: variable-size field must be last -------------------------

TEST(VariableSizeStructTests, varSizeFieldNotLastRejected) {
    auto src =
        "package test;\n"
        "public struct Bad {\n"
        "    String name;\n"
        "    int32 id;\n"                  // fixed-size field after variable-size
        "}\n"
        "public final class V {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    expectErrorId(src, "CAJETA_ERROR_VARSIZE_FIELD_NOT_LAST");
}
