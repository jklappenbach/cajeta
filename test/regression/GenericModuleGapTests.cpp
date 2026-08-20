//
// GenericModuleGapTests — cajeta-llama 2.2.4: the two compiler gaps recorded
// against a generic `Module<T>`/`Parameter<T>` (nucleo-nn-optim 8.2.2 ledger,
// 2026-07-21; named in dev.cajeta.ml `nn/Module.cajeta`'s class doc):
//
//   (a) `instanceof` / cast with an IDENTIFIER template argument mis-parses
//       inside a template body — the relational chain wins over the type
//       parse (`o instanceof ArrayList<T> l` reads as comparisons). Literal
//       arguments like `<int32>` were always fine.
//   (b) a concrete class extending an INSTANTIATED template base does not
//       resolve inherited members.
//
// These tests assert the CORRECT behavior — red while a gap stands, green
// with the fix, unedited (the NestedClassResolutionTests discipline). The
// gaps were recorded at v0.9.5; re-measured 2026-08-20 against this branch:
//
//   * The CAST half of (a) PASSES unedited — fixed somewhere since v0.9.5.
//     It stays enabled as the regression pin.
//   * The INSTANCEOF half of (a) still failed, and is FIXED HERE: in the
//     grammar's `(typeType | pattern)`, ALL(*) takes the lower alternative
//     whenever both are viable — and with an identifier argument both ARE:
//     typeType can stop at `ArrayList` and hand `< T > l` to the relational
//     alternatives (`(o instanceof ArrayList) < T > l`), so codegen died on
//     a valueless `<`. A keyword argument kills the relational path, which
//     is why `<int32>` never tripped it. Reordering to `(pattern | typeType)`
//     resolves it: `pattern` needs the trailing binder identifier, so the
//     plain form still falls through to typeType.
//   * (b) still stands, and is DOCUMENTED, not fixed — it is TPL-5, a known
//     v1 limitation recorded in TemplateInstantiator.cpp: the extends
//     clause's typeArguments are captured only for `implements`
//     (CajetaLlvmVisitor.h buildClassLike, `which == 1`) and silently
//     dropped for `extends`, so the subclass links to the UNINSTANTIATED
//     template, which has no materialized members. The fix (carry the args,
//     resolve the parent through the instantiation cache, then layout /
//     vtable / ctor consequences) is template-system work, out of scope for
//     cajeta-llama 2.2.4. DISABLED_ until TPL-5 lands; it should go green
//     with no edit.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn ? fn() : -100;
}

}  // namespace

// (a) — `instanceof` with an identifier template argument, inside a template
// body, parses as a type test and binds the guard variable.
TEST(GenericModuleGapTests, instanceofIdentifierTemplateArgInTemplateBody) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Holder<T> {\n"
        "    public int32 countIfList(Object o) {\n"
        "        if (o instanceof ArrayList<T> l) {\n"
        "            return l.count();\n"
        "        }\n"
        "        return -1;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> a = heap ArrayList<int32>();\n"
        "        a.add(7);\n"
        "        a.add(9);\n"
        "        Holder<int32> h = heap Holder<int32>();\n"
        "        return h.countIfList(a);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);
}

// (a) cast form — `(ArrayList<T>) o` inside a template body is a cast, not a
// parenthesized relational chain.
TEST(GenericModuleGapTests, castIdentifierTemplateArgInTemplateBody) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public class Caster<T> {\n"
        "    public int32 countOf(Object o) {\n"
        "        ArrayList<T> l = (ArrayList<T>) o;\n"
        "        return l.count();\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        ArrayList<int32> a = heap ArrayList<int32>();\n"
        "        a.add(4);\n"
        "        Caster<int32> c = heap Caster<int32>();\n"
        "        return c.countOf(a);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// (b) — a concrete class extending an INSTANTIATED template base resolves the
// base's members (method call on the derived static type).
TEST(GenericModuleGapTests, DISABLED_concreteExtendsInstantiatedTemplateResolvesMembers) {
    std::string src =
        "package test;\n"
        "public class Cell<T> {\n"
        "    T value;\n"
        "    public Cell() { }\n"
        "    public void put(T v) { this.value = v; }\n"
        "    public T get() { return this.value; }\n"
        "}\n"
        "public final class IntCell extends Cell<int32> {\n"
        "    public IntCell() { }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        IntCell c = heap IntCell();\n"
        "        c.put(41);\n"
        "        return c.get() + 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 42);
}
