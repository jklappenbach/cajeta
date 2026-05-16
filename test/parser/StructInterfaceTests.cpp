//
// Session 9 — struct implements interface; per-(struct, interface) vtable
// synthesis. The through-interface dispatch path itself (call through a
// fat-pointer interface value) is Sessions 10–11; this file pins the
// declaration shape, the implemented-interfaces metadata, and signature
// compatibility checks at the struct/interface boundary.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

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

// ---------------------------------------------------------------------
// S9.1 — `struct Foo implements Interface { ... }` parses, and a direct
// call on the concrete struct still works (no vtable hit; same path
// S8 wired up).
// ---------------------------------------------------------------------

TEST(StructInterfaceTests, structImplementsOneInterface) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "public struct Hi implements Greeter {\n"
        "    int32 base;\n"
        "    public int32 greet() { return this.base + 1; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Hi h = Hi { base: 10 };\n"
        "        return h.greet();\n"  // direct call, no vtable
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 11);
}

// ---------------------------------------------------------------------
// S9.2 — synthesize a per-(struct, interface) vtable global. Each
// pair gets its own static global containing function pointers to
// the struct's concrete implementations of the interface's methods,
// in interface-declaration order. Through-interface dispatch
// (Sessions 10–11) loads from this global into the tagged fat
// pointer at interface-value construction time.
// ---------------------------------------------------------------------

// Direct call still works when the struct implements two interfaces.
// Pins that the extra metadata + dual vtable globals don't disturb the
// direct-call path's signature/dispatch.
TEST(StructInterfaceTests, structImplementsTwoInterfaces) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public interface Counted { public int32 count(); }\n"
        "public struct Both implements Greeter, Counted {\n"
        "    int32 base;\n"
        "    public int32 greet() { return this.base + 1; }\n"
        "    public int32 count() { return this.base * 2; }\n"
        "}\n"
        "public final class S {\n"
        "    public static int32 run() {\n"
        "        Both b = Both { base: 10 };\n"
        "        return b.greet() + b.count();\n"  // 11 + 20 = 31
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}

// ---------------------------------------------------------------------
// S9.3 — vtable layout matches the interface's method order; signature
// compatibility checked at struct declaration. Two error IDs surface
// the failure modes cleanly.
// ---------------------------------------------------------------------

// Missing method: the interface requires `greet()` but the struct
// provides nothing matching the name. Vtable synthesis raises
// CAJETA_ERROR_INTERFACE_METHOD_NOT_IMPLEMENTED.
TEST(StructInterfaceTests, rejectMissingInterfaceMethod) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public struct Hi implements Greeter {\n"
        "    int32 base;\n"
        "}\n"
        "public final class S { public static int32 run() { return 0; } }\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_INTERFACE_METHOD_NOT_IMPLEMENTED";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(),
            "CAJETA_ERROR_INTERFACE_METHOD_NOT_IMPLEMENTED");
    }
}

// Signature mismatch: struct has a `greet()` method but its return
// type or parameters differ from the interface's declaration. The
// name-match wins so the error points at the actual problem rather
// than the missing-method one.
TEST(StructInterfaceTests, rejectSignatureMismatch) {
    auto src =
        "package test;\n"
        "public interface Greeter { public int32 greet(); }\n"
        "public struct Hi implements Greeter {\n"
        "    int32 base;\n"
        "    public int64 greet() { return 7; }\n"  // wrong return type
        "}\n"
        "public final class S { public static int32 run() { return 0; } }\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_INTERFACE_METHOD_SIGNATURE_MISMATCH";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(),
            "CAJETA_ERROR_INTERFACE_METHOD_SIGNATURE_MISMATCH");
    }
}

// ---------------------------------------------------------------------
// S9.4 — reject interface declarations with method-level generics.
// The grammar accepts `<T>` syntactically (so we can surface a clean
// error rather than ANTLR's "no viable alternative"), and the visitor
// throws CAJETA_ERROR_INTERFACE_METHOD_GENERIC when it sees one.
// ---------------------------------------------------------------------

TEST(StructInterfaceTests, rejectInterfaceMethodLevelGeneric) {
    auto src =
        "package test;\n"
        "public interface Searchable {\n"
        "    public <T> int32 find(int32 n);\n"
        "}\n"
        "public final class S { public static int32 run() { return 0; } }\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_INTERFACE_METHOD_GENERIC";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_INTERFACE_METHOD_GENERIC");
    }
}

// Signature mismatch via parameter list — interface declares
// `apply(int32)` but the struct's method takes nothing. Same error
// path as the return-type mismatch above.
TEST(StructInterfaceTests, rejectParameterMismatch) {
    auto src =
        "package test;\n"
        "public interface Applier { public int32 apply(int32 n); }\n"
        "public struct Hi implements Applier {\n"
        "    int32 base;\n"
        "    public int32 apply() { return this.base; }\n"  // missing param
        "}\n"
        "public final class S { public static int32 run() { return 0; } }\n";
    try {
        CajetaJit::compile(src, "test.S");
        FAIL() << "expected CAJETA_ERROR_INTERFACE_METHOD_SIGNATURE_MISMATCH";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(),
            "CAJETA_ERROR_INTERFACE_METHOD_SIGNATURE_MISMATCH");
    }
}
