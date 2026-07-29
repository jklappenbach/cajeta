//
// VEA-1 — view var-size element arrays: parser + model surface.
//
// Spec: specs/view-element-arrays-spec.md (view v1.1). A view may declare
// element-array fields:
//   - `V[] f;` where V is a view type (fixed-size or var-size elements), and
//   - `String[] f;` (the primitive var-size element case).
// Wire layout: u32 count prefix, then count elements back-to-back, each in
// V's own layout. Nested element arrays (`V[][]`, `String[][]`) are rejected
// with a located diagnostic.
//
// This file pins the declaration surface only. Construction validation +
// offset-table behavior is VEA-2 (ViewElementArrayCtorTests), element access
// is VEA-3 (ViewElementArrayAccessTests).
//
// NOTE: `V[]`/`String[]` as a *class* field remains rejected
// (CAJETA_ERROR_VIEW_AS_CLASS_FIELD, ViewAsClassFieldRejectionTests) — the
// new legality is only *inside a view declaration*.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

// --- Acceptance: view-typed element arrays ---------------------------------

// a.1 — a var-size element view (fixed fields + String) usable as V[] field.
TEST(ViewElementArrayTests, parsesViewArrayField) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view Delta {\n"
        "    int8   state;\n"
        "    int64  incarnation;\n"
        "    String name;\n"
        "}\n"
        "@BigEndian\n"
        "public view Msg {\n"
        "    int32   magic;\n"
        "    Delta[] deltas;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.S"));
}

// a.1 fixed-element flavor — V fixed-size (stride fast path's parse surface).
TEST(ViewElementArrayTests, parsesFixedElementViewArrayField) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view Pt { int32 x; int32 y; }\n"
        "@BigEndian\n"
        "public view Poly {\n"
        "    int32 kind;\n"
        "    Pt[]  points;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.S"));
}

// a.1 positional freedom — an element-array field is a var-size field like
// any other (S5b): fixed fields may follow it.
TEST(ViewElementArrayTests, fixedFieldAfterElementArrayAccepted) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view Delta { int8 state; String name; }\n"
        "@BigEndian\n"
        "public view Msg {\n"
        "    int32   magic;\n"
        "    Delta[] deltas;\n"
        "    int32   checksum;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.S"));
}

// --- Acceptance: String[] --------------------------------------------------

// a.2 — String[] view field.
TEST(ViewElementArrayTests, parsesStringArrayField) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view NameList {\n"
        "    int32    kind;\n"
        "    String[] names;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    EXPECT_NO_THROW(CajetaJit::compile(src, "test.S"));
}

// --- Rejection: nested element arrays --------------------------------------

// a.3 — V[][] rejected with a located diagnostic naming the field.
TEST(ViewElementArrayTests, rejectsNestedViewElementArrays) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view Pt { int32 x; int32 y; }\n"
        "@BigEndian\n"
        "public view Bad {\n"
        "    Pt[][] grid;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_VIEW_NESTED_ELEMENT_ARRAY";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_NESTED_ELEMENT_ARRAY");
    }
}

// a.3 — String[][] rejected the same way.
TEST(ViewElementArrayTests, rejectsNestedStringElementArrays) {
    auto src =
        "package test;\n"
        "@BigEndian\n"
        "public view Bad {\n"
        "    String[][] table;\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_VIEW_NESTED_ELEMENT_ARRAY";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_NESTED_ELEMENT_ARRAY");
    }
}
