// Multibinding (primavera-web plan Unit 3, primavera-spec §16 R2): an
// `@Inject ArrayList<T>` site receives every active @Component assignable to
// T, in canonical-name order, and an `@Inject HashMap<String, T>` site the
// same set keyed by component name. The elements are the graph's singletons,
// so the list holds borrows and dropping or clearing it frees nothing.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src, const std::string& entry) {
    auto jit = CajetaJit::compile(src, entry);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

int64_t runI64(const std::string& src, const std::string& entry) {
    auto jit = CajetaJit::compile(src, entry);
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

// Three shapes declared out of canonical order, so an ordered list is proof
// of ordering by name and not by declaration.
const char* kShapes =
    "package test;\n"
    "import cajeta.collection.ArrayList;\n"
    "import cajeta.collection.HashMap;\n"
    "public interface Shape {\n"
    "    public int32 tag();\n"
    "}\n"
    "@Component(name = \"tri\") public class Triangle implements Shape {\n"
    "    public int32 hits;\n"
    "    public Triangle() { this.hits = 0; return; }\n"
    "    public int32 tag() { this.hits = this.hits + 1; return 3; }\n"
    "}\n"
    "@Component public class Circle implements Shape {\n"
    "    public int32 hits;\n"
    "    public Circle() { this.hits = 0; return; }\n"
    "    public int32 tag() { this.hits = this.hits + 1; return 1; }\n"
    "}\n"
    "@Component(name = \"box\") @Profile(\"staging\") public class Square implements Shape {\n"
    "    public Square() { return; }\n"
    "    public int32 tag() { return 2; }\n"
    "}\n";

}  // namespace

// 3.1.1 + 3.1.2: every active implementation, canonical order, and the
// staging-only Square absent under the default profile.
TEST(MultibindingTests, listReceivesEveryActiveImplementationInCanonicalOrder) {
    std::string src = std::string(kShapes) +
        "@Component public class Registry {\n"
        "    @Inject ArrayList<Shape> shapes;\n"
        "    public Registry() { return; }\n"
        "}\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        Registry r = Registry.__cajeta_inject();\n"
        "        int32 n = r.shapes.count();\n"
        "        int32 first = r.shapes.get(0).tag();\n"
        "        int32 second = r.shapes.get(1).tag();\n"
        "        return n * 100 + first * 10 + second;\n"
        "    }\n"
        "}\n";
    // Circle then Triangle: 2 elements, tags 1 and 3, Square excluded.
    EXPECT_EQ(runI32(src, "test.Main"), 213);
}

// 3.1.1: no implementation means an empty list, not a missing-component error.
TEST(MultibindingTests, listIsEmptyWhenNothingImplementsTheType) {
    std::string src =
        "package test;\n"
        "import cajeta.collection.ArrayList;\n"
        "public interface Widget {\n"
        "    public int32 id();\n"
        "}\n"
        "@Component public class Registry {\n"
        "    @Inject ArrayList<Widget> widgets;\n"
        "    public Registry() { return; }\n"
        "}\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        Registry r = Registry.__cajeta_inject();\n"
        "        if (r.widgets == null) { return -1; }\n"
        "        return r.widgets.count();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Main"), 0);
}

// 3.1.3: the elements are the graph's singletons. A call through the list
// is visible through the component's own accessor, clearing the list frees
// nothing, and the live count moves only by the objects the graph created.
TEST(MultibindingTests, elementsAreTheGraphsSingletonsAndTheListHoldsBorrows) {
    std::string src = std::string(kShapes) +
        "@Component public class Registry {\n"
        "    @Inject ArrayList<Shape> shapes;\n"
        "    public Registry() { return; }\n"
        "}\n"
        "public final class Main {\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        Registry r = Registry.__cajeta_inject();\n"
        "        int64 created = Cajeta.liveCount() - base;\n"
        "        int32 t = r.shapes.get(0).tag();\n"
        "        Circle c = Circle.__cajeta_inject();\n"
        "        int64 sameObject = 0;\n"
        "        if (c.hits == 1) { sameObject = 1; }\n"
        "        int64 beforeClear = Cajeta.liveCount();\n"
        "        r.shapes.clear();\n"
        "        int64 freedByClear = beforeClear - Cajeta.liveCount();\n"
        "        int32 again = c.tag();\n"
        "        int64 stillAlive = 0;\n"
        "        if (c.hits == 2 && again == 1) { stillAlive = 1; }\n"
        "        return created * 1000 + sameObject * 100 + freedByClear * 10 + stillAlive;\n"
        "    }\n"
        "}\n";
    // created: Registry, its ArrayList and that list's Shape[] storage (an
    // interface-element array is a live object, a primitive array is not),
    // Circle, Triangle = 5 objects. Clearing frees none of them.
    EXPECT_EQ(runI64(src, "test.Main"), 5101);
}

// 3.1.4: the companion map is keyed by @Component name, and an unnamed
// component by its simple class name, so configuration can select by name.
TEST(MultibindingTests, mapIsKeyedByComponentName) {
    std::string src = std::string(kShapes) +
        "@Component public class Registry {\n"
        "    @Inject HashMap<String, Shape> byName;\n"
        "    public Registry() { return; }\n"
        "}\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        Registry r = Registry.__cajeta_inject();\n"
        "        int32 n = (int32) r.byName.count();\n"
        "        int32 tri = r.byName.get(\"tri\").tag();\n"
        "        int32 circle = r.byName.get(\"Circle\").tag();\n"
        "        int32 missing = 0;\n"
        "        if (r.byName.containsKey(\"box\")) { missing = 1; }\n"
        "        return n * 1000 + tri * 100 + circle * 10 + missing;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Main"), 2310);
}

// A list site does not disturb single-target resolution beside it, and a
// single site over a many-implementation type still needs a qualifier.
TEST(MultibindingTests, singleSitesBesideAListStillResolveByName) {
    std::string src = std::string(kShapes) +
        "@Component public class Registry {\n"
        "    @Inject ArrayList<Shape> shapes;\n"
        "    @Inject(name = \"tri\") Shape chosen;\n"
        "    public Registry() { return; }\n"
        "}\n"
        "public final class Main {\n"
        "    public static int32 run() {\n"
        "        Registry r = Registry.__cajeta_inject();\n"
        "        return r.shapes.count() * 10 + r.chosen.tag();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src, "test.Main"), 23);
}
