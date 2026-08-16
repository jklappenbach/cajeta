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

// Array of views as a class field.

// HashMap<int32, view> — V[] vals becomes view[], caught via the
// templated container's prototype build.

// HashMap<view, int32> — K[] keys becomes view[].

// ArrayList<view> — T[] elements becomes view[].
TEST(ViewAsClassFieldRejectionTests, arrayListOfViewRejected) {
    auto src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "@BigEndian view Pt { int32 x; int32 y; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<Pt> a = heap ArrayList<Pt>(16);\n"
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

// Optional<view> — single T-typed field, caught by the direct-view branch.

// Sanity: nested views are NOT rejected — a view containing a view
// field is legitimate per Views.md § Nested views (the inner lays
// out inline within the outer's buffer). Mirrors NestedViewTests
// patterns (int32[] buffer rather than byte[]).

// Sanity: classes without view fields keep compiling fine.
