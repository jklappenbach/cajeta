// Probe: templated-interface-typed parameter dispatch.
//
// Bug shape (documented in StreamParallelism.Examples.md § 7.5):
// a static templated method that takes a templated-interface
// parameter and calls a method on it dispatches to the abstract base
// rather than the concrete implementer. Manifests as
// `Stream<T>.next()` returning an empty Optional even though the
// caller passed an `ArrayStream<T>` that should yield elements.
//
// Each TEST below isolates one variant. The three "works" probes
// confirm the bug shape narrows to ONE specific shape — a templated
// interface formal parameter receiving a concrete implementer at the
// call site, called inside a method-templated static.

#include "gtest/gtest.h"
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

// Baseline 1: walk<T>(MyConcrete<T>) — concrete-class formal,
// concrete-class arg. Plain virtual dispatch; expected to pass.
TEST(TemplatedInterfaceParamProbe, concreteClassFormal) {
    auto src =
        "package test;\n"
        "public class MyBox<T> {\n"
        "    int32 v;\n"
        "    public MyBox(int32 vv) { this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 walk<T>(MyBox<T> s) { return s.read(); }\n"
        "    public static int32 run() {\n"
        "        MyBox<int32> b = heap MyBox<int32>(42);\n"
        "        return D.walk<int32>(b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}

// Baseline 2: walk<T>(MyBase<T>) — concrete-base-class formal,
// concrete-derived-class arg. Standard inheritance dispatch; expected
// to pass.
TEST(TemplatedInterfaceParamProbe, abstractBaseFormalConcreteImpl) {
    auto src =
        "package test;\n"
        "public class MyBase<T> {\n"
        "    public int32 read() { return -1; }\n"
        "}\n"
        "public class MyImpl<T> extends MyBase<T> {\n"
        "    int32 v;\n"
        "    public MyImpl(int32 vv) { super(); this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 walk<T>(MyBase<T> s) { return s.read(); }\n"
        "    public static int32 run() {\n"
        "        MyImpl<int32> b = heap MyImpl<int32>(99);\n"
        "        return D.walk<int32>(b);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// The bug: walk<T>(IFoo<T>) — interface formal, concrete-impl arg.
// Expected to return 7 (impl's read()); pre-fix actually returns -1
// or 0 (calls a default/inherited Stream.next() shape, returning an
// empty/dummy value).
TEST(TemplatedInterfaceParamProbe, interfaceFormalConcreteImpl) {
    auto src =
        "package test;\n"
        "public interface IFoo<T> {\n"
        "    public int32 read();\n"
        "}\n"
        "public class FooImpl<T> implements IFoo<T> {\n"
        "    int32 v;\n"
        "    public FooImpl(int32 vv) { this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 walk<T>(IFoo<T> s) { return s.read(); }\n"
        "    public static int32 run() {\n"
        "        FooImpl<int32> b = heap FooImpl<int32>(7);\n"
        "        return D.walk<int32>(b);\n"
        "    }\n"
        "}\n";
    try {
        EXPECT_EQ(runI32(src), 7);
    } catch (cajeta::Exception& e) {
        FAIL() << "cajeta::Exception " << e.getErrorId() << ": " << e.getMessage();
    } catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}

// The same bug shape with a non-templated interface, to narrow whether
// the breakage is interface dispatch OR templated-interface dispatch
// specifically.
TEST(TemplatedInterfaceParamProbe, nonTemplatedInterfaceFormal) {
    auto src =
        "package test;\n"
        "public interface IFoo {\n"
        "    public int32 read();\n"
        "}\n"
        "public class FooImpl implements IFoo {\n"
        "    int32 v;\n"
        "    public FooImpl(int32 vv) { this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 walk<T>(IFoo s) { return s.read(); }\n"
        "    public static int32 run() {\n"
        "        FooImpl b = heap FooImpl(13);\n"
        "        return D.walk<int32>(b);\n"
        "    }\n"
        "}\n";
    try {
        EXPECT_EQ(runI32(src), 13);
    } catch (cajeta::Exception& e) {
        FAIL() << "cajeta::Exception " << e.getErrorId() << ": " << e.getMessage();
    } catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}

// Simplest possible shape: no templates anywhere, just a non-
// templated interface formal receiving a concrete impl. Should
// trivially dispatch via the fat pointer. If this fails too, the bug
// is "any class→interface arg-passing".
TEST(TemplatedInterfaceParamProbe, plainInterfaceFormalPlainStatic) {
    auto src =
        "package test;\n"
        "public interface IFoo {\n"
        "    public int32 read();\n"
        "}\n"
        "public class FooImpl implements IFoo {\n"
        "    int32 v;\n"
        "    public FooImpl(int32 vv) { this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 walk(IFoo s) { return s.read(); }\n"
        "    public static int32 run() {\n"
        "        FooImpl b = heap FooImpl(33);\n"
        "        return D.walk(b);\n"
        "    }\n"
        "}\n";
    try {
        EXPECT_EQ(runI32(src), 33);
    } catch (cajeta::Exception& e) {
        FAIL() << "cajeta::Exception " << e.getErrorId() << ": " << e.getMessage();
    } catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}

// And the templated-interface formal in a non-templated static, to
// narrow whether method-template instantiation is part of the cause.
TEST(TemplatedInterfaceParamProbe, interfaceFormalNonTemplatedStatic) {
    auto src =
        "package test;\n"
        "public interface IFoo<T> {\n"
        "    public int32 read();\n"
        "}\n"
        "public class FooImpl<T> implements IFoo<T> {\n"
        "    int32 v;\n"
        "    public FooImpl(int32 vv) { this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 walk(IFoo<int32> s) { return s.read(); }\n"
        "    public static int32 run() {\n"
        "        FooImpl<int32> b = heap FooImpl<int32>(21);\n"
        "        return D.walk(b);\n"
        "    }\n"
        "}\n";
    try {
        EXPECT_EQ(runI32(src), 21);
    } catch (cajeta::Exception& e) {
        FAIL() << "cajeta::Exception " << e.getErrorId() << ": " << e.getMessage();
    } catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}

// HashMapKeyStream-shape: a 2-type-param class extends a 1-type-param
// class AND implements a 1-type-param interface that itself extends
// that same class. Tests the diamond instantiation around Splittable<K>
// extending Stream<K> while the impl also extends Stream<K>.
TEST(TemplatedInterfaceParamProbe, twoTypeParamExtendsAndImplementsDiamond) {
    auto src =
        "package test;\n"
        "public class Base<T> {\n"
        "    public int32 read() { return 0; }\n"
        "}\n"
        "public interface IFoo<T> extends Base<T> {\n"
        "    public int32 split();\n"
        "}\n"
        "public class FooImpl<K, V> extends Base<K> implements IFoo<K> {\n"
        "    int32 v;\n"
        "    public FooImpl(int32 vv) { this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "    public int32 split() { return 99; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        FooImpl<int32, int32> b = heap FooImpl<int32, int32>(77);\n"
        "        return b.read() + b.split();\n"
        "    }\n"
        "}\n";
    try {
        EXPECT_EQ(runI32(src), 176);
    } catch (cajeta::Exception& e) {
        FAIL() << "cajeta::Exception " << e.getErrorId() << ": " << e.getMessage();
    } catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}

// Probe: a 2-type-param class implements a 1-type-param interface
// where the iface argument is ITSELF a 2-type-param parameterized
// type built out of both class params (the HashMapEntryStream<K, V>
// shape implementing Splittable<Pair<K, V>>). This was observed to
// hang.
TEST(TemplatedInterfaceParamProbe, twoTypeParamImplementsParameterizedIfaceArg) {
    auto src =
        "package test;\n"
        "public class Pair<A, B> {\n"
        "    public A a;\n"
        "    public B b;\n"
        "    public Pair(A aa, B bb) { this.a = aa; this.b = bb; }\n"
        "}\n"
        "public class Base<T> {\n"
        "    public int32 read() { return 0; }\n"
        "}\n"
        "public interface IFoo<T> extends Base<T> {\n"
        "    public int32 split();\n"
        "}\n"
        "public class FooImpl<K, V> extends Base<Pair<K, V>> implements IFoo<Pair<K, V>> {\n"
        "    int32 v;\n"
        "    public FooImpl(int32 vv) { this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "    public int32 split() { return 100; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        FooImpl<int32, int32> b = heap FooImpl<int32, int32>(88);\n"
        "        return b.read() + b.split();\n"
        "    }\n"
        "}\n";
    try {
        EXPECT_EQ(runI32(src), 188);
    } catch (cajeta::Exception& e) {
        FAIL() << "cajeta::Exception " << e.getErrorId() << ": " << e.getMessage();
    } catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}

// 2-type-param class implements a 1-type-param interface, passing
// through only the FIRST type parameter. This is the shape that
// HashMapKeyStream<K, V> needs in order to declare `implements
// Splittable<K>`. Without it, parallel HashMap streams can't fork.
TEST(TemplatedInterfaceParamProbe, twoTypeParamImplementsOneTypeParamIface) {
    auto src =
        "package test;\n"
        "public interface IFoo<T> {\n"
        "    public int32 read();\n"
        "}\n"
        "public class FooImpl<K, V> implements IFoo<K> {\n"
        "    int32 v;\n"
        "    public FooImpl(int32 vv) { this.v = vv; }\n"
        "    public int32 read() { return this.v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        FooImpl<int32, int32> b = heap FooImpl<int32, int32>(55);\n"
        "        return b.read();\n"
        "    }\n"
        "}\n";
    try {
        EXPECT_EQ(runI32(src), 55);
    } catch (cajeta::Exception& e) {
        FAIL() << "cajeta::Exception " << e.getErrorId() << ": " << e.getMessage();
    } catch (const std::exception& e) {
        FAIL() << "std::exception: " << e.what();
    }
}
