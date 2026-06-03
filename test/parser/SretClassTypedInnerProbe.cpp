//
// Regression guard for the sret + class-typed inner pattern in stream
// pipelines (HashMapEntryStream and adjacent). Both shapes work after #67:
//   - `return stack Optional<C>(true, heap C(...));`  (inlined ctor)
//   - `C c = heap C(...); return stack Optional<C>(true, c);`  (named local)
// The fix in #67 marked `Optional`'s value formal as `#T` and added the
// ctor-call-site transfer in CreatorRest.cpp (mirror of MCE.cpp:4091+)
// so passing a class-typed local into the ctor deactivates the caller's
// drop entry — the new Optional then becomes the sole owner of the inner
// allocation.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

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

// Named-local shape: heap-class local consumed by stack-Optional ctor
// via sret, with the caller-side `#p` acknowledgement that Phase 2
// of #68 requires. Pre-#67 this double-freed; #67 made it work and
// #68 Phase 2 demands the `#` so the transfer is visible at the call
// site instead of happening silently.
TEST(SretClassTypedInnerProbe, namedLocalHeapClassIntoStackOptional) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Pair {\n"
        "    public int32 x;\n"
        "    public Pair(int32 x) { this.x = x; }\n"
        "}\n"
        "public class Src {\n"
        "    public Optional<Pair> next() {\n"
        "        Pair p = heap Pair(7);\n"
        "        return stack Optional<Pair>(true, #p);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Src s = heap Src();\n"
        "        Optional<Pair> o = s.next();\n"
        "        return o.get().x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Inline ctor shape: heap construction folded into the Optional ctor
// call. Worked even before #67 (no intermediate local with a drop entry).
TEST(SretClassTypedInnerProbe, inlineHeapCtorIntoStackOptional) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Pair {\n"
        "    public int32 x;\n"
        "    public Pair(int32 x) { this.x = x; }\n"
        "}\n"
        "public class Src {\n"
        "    public Optional<Pair> next() {\n"
        "        return stack Optional<Pair>(true, heap Pair(7));\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Src s = heap Src();\n"
        "        Optional<Pair> o = s.next();\n"
        "        return o.get().x;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Multi-call shape — mirrors HashMap.entries().count() loop. Uses the
// inline ctor pattern so the loop completes without double-free.
TEST(SretClassTypedInnerProbe, repeatedSretCallNoDoubleFree) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class Pair {\n"
        "    public int32 x;\n"
        "    public Pair(int32 x) { this.x = x; }\n"
        "}\n"
        "public class Src {\n"
        "    int32 i;\n"
        "    public Src() { this.i = 0; }\n"
        "    public Optional<Pair> next() {\n"
        "        if (this.i < 4) {\n"
        "            int32 v = this.i;\n"
        "            this.i = this.i + 1;\n"
        "            return stack Optional<Pair>(true, heap Pair(v));\n"
        "        }\n"
        "        return stack Optional<Pair>(false, null);\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Src s = heap Src();\n"
        "        int32 count = 0;\n"
        "        Optional<Pair> o = s.next();\n"
        "        while (o.isPresent()) {\n"
        "            count = count + 1;\n"
        "            o = s.next();\n"
        "        }\n"
        "        return count;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}
