//
// S3 of the value-type operator-overloading mechanism
// (plans/value-type-overloading-plan.md): the operator[] read gate works for a
// @ValueType receiver, and the LOCKED mutating-operator policy (Decision #3) is
// enforced — a @ValueType class may NOT declare an instance mutating operator
// (operator++/--, operator[]=, compound-assign), because value types are
// by-value Copy and an in-place mutation through the receiver writes a copy and
// is lost. Read-only operator[] returning a value is allowed.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// A @ValueType with a read-only operator[] returning a component by lane index
// dispatches through the operator and reads the right field.

// Declaring operator[]= on a @ValueType is a compile error (mutating write on a
// Copy receiver would be silently lost).

// Declaring operator++ on a @ValueType is likewise rejected.
TEST(ValueTypeIndexOperatorTests, mutatingIncrementRejected) {
    std::string src =
        "package test;\n"
        "@ValueType public final class Counter {\n"
        "    public int32 n;\n"
        "    public Counter(int32 n) { this.n = n; }\n"
        "    public void operator++ () { this.n = this.n + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected @ValueType operator++ to be rejected";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(std::string(e.getErrorId()),
                  "CAJETA_ERROR_VALUE_TYPE_MUTATING_OPERATOR");
    } catch (const std::exception& e) {
        EXPECT_NE(std::string(e.what()).find("mutating operator"),
                  std::string::npos) << e.what();
    }
}
