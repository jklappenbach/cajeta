//
// A10: name-qualifier resolution for @Inject through interfaces.
//
// When two @Component classes implement the same interface, the
// consumer disambiguates with @Inject(name = "..."). The resolver
// matches consumer name to the @Component(name = ...) qualifier;
// A9's codegen then routes the field-assignment call through the
// selected impl's __cajeta_inject().
//
// The interface-impl walk lives in resolveDependency (third
// fallback after byCanonical and byShort) — see
// CajetaModule::resolveDependencyGraph.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src, const std::string& fqEntryClass) {
    auto jit = CajetaJit::compile(src, fqEntryClass);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// Two impls of one interface; consumer @Inject(name="disk") selects
// the disk variant. Validates via virtual call through the
// interface-typed field: pers.store(5) hits Disk's body and returns
// 5 + 100 = 105. If "memory" had been selected instead the result
// would be 1005.
TEST(NamedInjectTests, namedInjectSelectsDisk) {
    auto src =
        "package test;\n"
        "public interface Persister {\n"
        "    public int32 store(int32 x);\n"
        "}\n"
        "@Component(name = \"disk\") public class Disk implements Persister {\n"
        "    public Disk() { return; }\n"
        "    public int32 store(int32 x) { return x + 100; }\n"
        "}\n"
        "@Component(name = \"memory\") public class Memory implements Persister {\n"
        "    public Memory() { return; }\n"
        "    public int32 store(int32 x) { return x + 1000; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject(name = \"disk\") Persister p;\n"
        "    public Service() { return; }\n"
        "    public static int32 run() {\n"
        "        Service s = __cajeta_inject();\n"
        "        Persister pers = s.p;\n"
        "        return pers.store(5);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Service"), 105);
}

// Symmetric to the above: @Inject(name="memory") selects the
// memory variant. Confirms the name match is real (not always
// picking the first registered).
TEST(NamedInjectTests, namedInjectSelectsMemory) {
    auto src =
        "package test;\n"
        "public interface Persister {\n"
        "    public int32 store(int32 x);\n"
        "}\n"
        "@Component(name = \"disk\") public class Disk implements Persister {\n"
        "    public Disk() { return; }\n"
        "    public int32 store(int32 x) { return x + 100; }\n"
        "}\n"
        "@Component(name = \"memory\") public class Memory implements Persister {\n"
        "    public Memory() { return; }\n"
        "    public int32 store(int32 x) { return x + 1000; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject(name = \"memory\") Persister p;\n"
        "    public Service() { return; }\n"
        "    public static int32 run() {\n"
        "        Service s = __cajeta_inject();\n"
        "        Persister pers = s.p;\n"
        "        return pers.store(5);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Service"), 1005);
}

// Two @Inject sites in one consumer, each with a different name —
// confirms the resolver tracks the per-field name qualifier and
// codegen wires each field to its own resolved impl. (5 + 100) +
// (5 + 1000) = 1110.
TEST(NamedInjectTests, twoNamedInjectsCoexist) {
    auto src =
        "package test;\n"
        "public interface Persister {\n"
        "    public int32 store(int32 x);\n"
        "}\n"
        "@Component(name = \"disk\") public class Disk implements Persister {\n"
        "    public Disk() { return; }\n"
        "    public int32 store(int32 x) { return x + 100; }\n"
        "}\n"
        "@Component(name = \"memory\") public class Memory implements Persister {\n"
        "    public Memory() { return; }\n"
        "    public int32 store(int32 x) { return x + 1000; }\n"
        "}\n"
        "@Component public class Service {\n"
        "    @Inject(name = \"disk\") Persister diskP;\n"
        "    @Inject(name = \"memory\") Persister memP;\n"
        "    public Service() { return; }\n"
        "    public static int32 run() {\n"
        "        Service s = __cajeta_inject();\n"
        "        Persister d = s.diskP;\n"
        "        Persister m = s.memP;\n"
        "        return d.store(5) + m.store(5);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Service"), 1110);
}

// Single impl, no name qualifier on either side. The default-
// resolution path picks the sole candidate. No name plumbing
// involved; this exercises the interface fallback without the
// disambiguation logic firing.
TEST(NamedInjectTests, singleImplResolvesWithoutName) {
    auto src =
        "package test;\n"
        "public interface Greeter {\n"
        "    public int32 greet();\n"
        "}\n"
        "@Component public class Hello implements Greeter {\n"
        "    public Hello() { return; }\n"
        "    public int32 greet() { return 42; }\n"
        "}\n"
        "@Component public class Caller {\n"
        "    @Inject Greeter g;\n"
        "    public Caller() { return; }\n"
        "    public static int32 run() {\n"
        "        Caller c = __cajeta_inject();\n"
        "        Greeter gr = c.g;\n"
        "        return gr.greet();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Caller"), 42);
}
