// uniform-transfer 2.3 — looking up a key you SURRENDERED.
//
// A map that owns its keys takes `#K`, so the caller's local is moved-from
// and reading it again is CAJETA_ERROR_USE_AFTER_MOVE. That raises the
// question these tests answer: how do you ever look the entry up again?
//
// The answer depends on how the key type compares, and the difference is
// sharp enough to be worth pinning:
//
//   - A VALUE-hashed key (String, primitives) can be re-derived. Look up
//     with a fresh equal key and the entry is found.
//   - An IDENTITY-hashed key (a plain user class, which is Cajeta's
//     default) CANNOT be re-derived — a fresh, field-identical instance is
//     a different key. The only surviving handle is a borrow taken BEFORE
//     the move.
//
// This is the one place Cajeta's ownership story differs materially from
// Rust's. Rust makes `map.get(&fresh_equal_key)` work because `Eq`/`Hash`
// are structural; Cajeta's default is identity, so surrendering a class
// key without first aliasing it makes that entry unreachable for lookup.
// Callers who need both must alias first.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    return jit->lookup<int32_t (*)()>("run")();
}
const char* kTag =
    "package test;\n"
    "import cajeta.collection.HashMap;\n"
    "public class Tag {\n"
    "    public int32 id;\n"
    "    public Tag(int32 i) { this.id = i; }\n"
    "}\n";
}  // namespace

// A borrow taken BEFORE the move survives it and still finds the entry:
// the map now owns the object, and the alias points at that same object.
TEST(OwnedKeyLookupTests, aliasTakenBeforeMoveStillLooksUp) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>();\n"
        "        Tag t = heap Tag(7);\n"
        "        Tag alias = t;\n"
        "        m.put(#t, 42);\n"
        "        return m.get(alias);\n"
        "    }\n"
        "}\n"), 42);
}

// A fresh, field-identical key does NOT find it — class keys hash by
// identity. Pinned as a MISS (get returns the zero value) so that if
// structural equality ever lands for classes, this test fails loudly and
// the decision gets made deliberately rather than by drift.
TEST(OwnedKeyLookupTests, freshEqualClassKeyDoesNotFindSurrenderedEntry) {
    EXPECT_EQ(runI32(std::string(kTag) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<Tag, int32> m = heap HashMap<Tag, int32>();\n"
        "        Tag t = heap Tag(7);\n"
        "        m.put(#t, 42);\n"
        "        Tag probe = heap Tag(7);\n"
        "        return m.get(probe);\n"
        "    }\n"
        "}\n"), 0);
}

// A String key IS value-hashed, so the fresh-literal lookup that fails for
// a class key works here. This is the common case and the reason the
// migration is tolerable in practice.
TEST(OwnedKeyLookupTests, freshEqualStringKeyFindsSurrenderedEntry) {
    EXPECT_EQ(runI32(
        "package test;\n"
        "import cajeta.collection.HashMap;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        HashMap<String, int32> m = heap HashMap<String, int32>();\n"
        "        String s = \"alpha\";\n"
        "        m.put(#s, 42);\n"
        "        return m.get(\"alpha\");\n"
        "    }\n"
        "}\n"), 42);
}
