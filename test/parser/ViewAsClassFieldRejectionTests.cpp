// Rejection tests for `CAJETA_ERROR_VIEW_AS_CLASS_FIELD`. Views are
// buffer overlays with borrowed lifetime — embedding one in a class
// field (directly or via an array element) creates an unresolvable
// lifetime hazard and breaks the compiler's calling convention (see
// Views.md § Errors caught statically).
//
// The check fires in `CajetaClass::generatePrototype` and naturally
// covers both hand-rolled classes and templated instantiations like
// `HashMap<view, X>` or `Optional<view>` — the instantiation's
// fields are checked when the prototype is built.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

// Direct view-typed field on a class — clearest violation.
TEST(ViewAsClassFieldRejectionTests, directViewFieldRejected) {
    auto src =
        "package test;\n"
        "@BigEndian view Pt { int32 x; int32 y; }\n"
        "public class Holder {\n"
        "    public Pt p;\n"
        "    public Holder() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VIEW_AS_CLASS_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
    }
}

// Array of views as a class field.
TEST(ViewAsClassFieldRejectionTests, arrayOfViewFieldRejected) {
    auto src =
        "package test;\n"
        "@BigEndian view Pt { int32 x; int32 y; }\n"
        "public class Holder {\n"
        "    public Pt[] arr;\n"
        "    public Holder() { return; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VIEW_AS_CLASS_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
    }
}

// HashMap<int32, view> — V[] vals becomes view[], caught via the
// templated container's prototype build.
TEST(ViewAsClassFieldRejectionTests, hashMapWithViewValueRejected) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "@BigEndian view Pt { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<int32, Pt> m = new HashMap<int32, Pt>(16);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VIEW_AS_CLASS_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
    }
}

// HashMap<view, int32> — K[] keys becomes view[].
TEST(ViewAsClassFieldRejectionTests, hashMapWithViewKeyRejected) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "@BigEndian view Pt { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Pt, int32> m = new HashMap<Pt, int32>(16);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VIEW_AS_CLASS_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
    }
}

// ArrayList<view> — T[] elements becomes view[].
TEST(ViewAsClassFieldRejectionTests, arrayListOfViewRejected) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "@BigEndian view Pt { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Pt> a = new ArrayList<Pt>(16);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VIEW_AS_CLASS_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
    }
}

// HashSet<view> — wraps HashMap<T, int8>; same K[] -> view[] path.
TEST(ViewAsClassFieldRejectionTests, hashSetOfViewRejected) {
    auto src =
        "package test;\n"
        "import cajeta.collection.HashSet;\n"
        "@BigEndian view Pt { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashSet<Pt> s = new HashSet<Pt>(16);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VIEW_AS_CLASS_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
    }
}

// Optional<view> — single T-typed field, caught by the direct-view branch.
TEST(ViewAsClassFieldRejectionTests, optionalOfViewRejected) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "@BigEndian view Pt { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Optional<Pt> o = Optional.None<Pt>();\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_VIEW_AS_CLASS_FIELD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_VIEW_AS_CLASS_FIELD");
    }
}

// Sanity: nested views are NOT rejected — a view containing a view
// field is legitimate per Views.md § Nested views (the inner lays
// out inline within the outer's buffer). Mirrors NestedViewTests
// patterns (int32[] buffer rather than byte[]).
TEST(ViewAsClassFieldRejectionTests, nestedViewsAllowed) {
    auto src =
        "package test;\n"
        "@HostEndian public view Pt { int32 x; int32 y; }\n"
        "@HostEndian public view Line { Pt start; Pt end; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] bytes = new int32[4];\n"
        "        Line l = Line(bytes);\n"
        "        l.start.x = 7;\n"
        "        return l.start.x;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

// Sanity: classes without view fields keep compiling fine.
TEST(ViewAsClassFieldRejectionTests, ordinaryClassFieldsUnaffected) {
    auto src =
        "package test;\n"
        "public class Counter {\n"
        "    public int32 n;\n"
        "    public Counter() { this.n = 0; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Counter c = heap Counter();\n"
        "        c.n = 42;\n"
        "        return c.n;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}
