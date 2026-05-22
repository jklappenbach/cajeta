// Templated-interface v2 — pins the gaps left by v1
// (TemplatedInterfaceTests.cpp).
//
// v1 covered:
//   - declaration of `interface Foo<T>`
//   - non-templated implementer (`class Enc implements Encoder<M>`)
//   - dispatch through interface-typed local for non-templated impl
//
// v2 covers:
//   - templated implementer (`class Box<T> implements Holder<T>`):
//     the instantiator now propagates the implements clause with
//     substitution. Pre-fix, the instantiator walked only the
//     extends typeList and silently dropped implements entries —
//     Box<int32> ended up with empty implementedInterfaces, so a
//     `Holder<int32>` upcast had no vtable and dispatch crashed.
//   - multi-type-param interface (`KV<K, V>`) — already worked, pinned.
//   - templated interface extending another templated interface:
//     three pieces had to land together — (a) interface
//     instantiation now resolves its own extends list with the
//     active substitution; (b) the interface branch of
//     generatePrototype calls resolveSuperClasses so the chain is
//     populated; (c) buildVirtualTable + synthesizeInterfaceVTables
//     both walk implementedInterfaces TRANSITIVELY so parent
//     interfaces in the chain also get vtable entries on the
//     implementing class.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Baseline: declare a templated implementer and call through the
// implementer's own type. Pre-fix this passed already.
TEST(TemplatedInterfaceV2Tests, templatedImplementerDeclareOnly) {
    auto src =
        "package test;\n"
        "public interface Holder<T> {\n"
        "    T get();\n"
        "}\n"
        "public class Box<T> implements Holder<T> {\n"
        "    public T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = heap Box<int32>(99);\n"
        "        return b.get();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// Assign Box<int32> into a Holder<int32>-typed local without
// dispatching — pre-fix the fat-pointer assembly stored null for
// the vtable because Box<int32>'s implementedInterfaces was empty.
TEST(TemplatedInterfaceV2Tests, templatedImplementerInterfaceAssignOnly) {
    auto src =
        "package test;\n"
        "public interface Holder<T> {\n"
        "    T get();\n"
        "}\n"
        "public class Box<T> implements Holder<T> {\n"
        "    public T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = heap Box<int32>(99);\n"
        "        Holder<int32> h = b;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// Dispatch through the interface-typed local — exercises the full
// per-(impl, iface) vtable path for the templated implementer.
TEST(TemplatedInterfaceV2Tests, templatedImplementerInterfaceDispatch) {
    auto src =
        "package test;\n"
        "public interface Holder<T> {\n"
        "    T get();\n"
        "}\n"
        "public class Box<T> implements Holder<T> {\n"
        "    public T value;\n"
        "    public Box(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box<int32> b = heap Box<int32>(99);\n"
        "        Holder<int32> h = b;\n"
        "        return h.get();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

// Multi-type-param interface threaded through dispatch.
TEST(TemplatedInterfaceV2Tests, multiTypeParamInterfaceDispatch) {
    auto src =
        "package test;\n"
        "public interface KV<K, V> {\n"
        "    K key();\n"
        "    V value();\n"
        "}\n"
        "public class IntPair implements KV<int32, int32> {\n"
        "    public IntPair() { }\n"
        "    public int32 key() { return 7; }\n"
        "    public int32 value() { return 42; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        KV<int32, int32> e = heap IntPair();\n"
        "        return e.key() + e.value();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 49);
}

// Templated interface extending another templated interface, with
// dispatch through the implementer's own type. Pins the no-upcast
// shape so we can distinguish parse/instantiate failure from
// upcast/vtable failure.
TEST(TemplatedInterfaceV2Tests, templatedInterfaceExtendsDeclareOnly) {
    auto src =
        "package test;\n"
        "public interface Reader<T> {\n"
        "    T read();\n"
        "}\n"
        "public interface Codec<T> extends Reader<T> {\n"
        "    int32 encode(T v);\n"
        "}\n"
        "public class IntCodec implements Codec<int32> {\n"
        "    public IntCodec() { }\n"
        "    public int32 read() { return 11; }\n"
        "    public int32 encode(int32 v) { return v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IntCodec c = heap IntCodec();\n"
        "        return c.read() + c.encode(20);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 32);
}

// Upcast to the parent interface only — assignment without dispatch.
TEST(TemplatedInterfaceV2Tests, templatedInterfaceExtendsUpcastAssign) {
    auto src =
        "package test;\n"
        "public interface Reader<T> {\n"
        "    T read();\n"
        "}\n"
        "public interface Codec<T> extends Reader<T> {\n"
        "    int32 encode(T v);\n"
        "}\n"
        "public class IntCodec implements Codec<int32> {\n"
        "    public IntCodec() { }\n"
        "    public int32 read() { return 11; }\n"
        "    public int32 encode(int32 v) { return v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IntCodec c = heap IntCodec();\n"
        "        Reader<int32> r = c;\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 0);
}

// Full chain: implementing class, upcast to a TEMPLATED parent
// interface, dispatch through it. The transitive walk in
// buildVirtualTable + synthesizeInterfaceVTables is what makes the
// Reader<int32>-typed dispatch resolve to IntCodec's read.
TEST(TemplatedInterfaceV2Tests, templatedInterfaceExtendsTemplatedInterface) {
    auto src =
        "package test;\n"
        "public interface Reader<T> {\n"
        "    T read();\n"
        "}\n"
        "public interface Codec<T> extends Reader<T> {\n"
        "    int32 encode(T v);\n"
        "}\n"
        "public class IntCodec implements Codec<int32> {\n"
        "    public IntCodec() { }\n"
        "    public int32 read() { return 11; }\n"
        "    public int32 encode(int32 v) { return v + 1; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IntCodec c = heap IntCodec();\n"
        "        Reader<int32> r = c;\n"
        "        return r.read() + c.encode(20);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 32);
}
