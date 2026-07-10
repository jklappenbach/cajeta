//
// element-ownership Unit 7 — borrow-mode confinement (spec §5, §8.2; plan
// 7.1.1-7.1.4). A borrow-instantiated container (any type argument left plain
// where the author marked `#K`) holds references it does not own, so the
// container VALUE is scope-confined: it may not be stored into a field,
// returned via `#`, or moved into an owning position (`#`-formal, `#`
// type argument — §8.2.2). Read-only use (plain formals, element borrows,
// mode-agnostic method templates) is unaffected, and an element borrow
// flowed into an owning position auto-materializes (Unit 3 path, §5.1.5).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

#include "cajeta/error/Exception.h"

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

std::string compileExpectError(const std::string& src,
                               const std::string& expectCode) {
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), expectCode);
        return e.getMessage();
    } catch (const std::exception& e) {
        return e.what();
    }
    ADD_FAILURE() << "expected a compile error";
    return "";
}

// Dual-mode container under test (same shape as the Unit 4B agreement
// tests): author-marked transfer mutator put(#K), read-only peek().
// `Vault<Elem>` (plain) is borrow-mode; `Vault<#Elem>` is owning.
const char* kVaultSrc =
    "package test;\n"
    "public class Elem { public int32 tag; public Elem(int32 t) { this.tag = t; } }\n"
    "public class Vault<K> {\n"
    "    public K store;\n"
    "    public void put(#K k) { this.store = k; }\n"
    "    public K peek() { return this.store; }\n"
    "}\n";

} // namespace

// ---------------------------------------------------------------------------
// 7.1.1 — the borrow-mode container value is scope-confined.
// ---------------------------------------------------------------------------

// Returning a borrow-mode container via `#` transfers it past the scope of
// the elements it borrows → compile error naming the fixes.
TEST(BorrowModeConfinementTests, hashReturnOfBorrowModeContainerIsConfined) {
    std::string src = std::string(kVaultSrc) +
        "public final class D {\n"
        "    public static #Vault<Elem> make() {\n"
        "        Vault<Elem> scratch = heap Vault<Elem>();\n"
        "        return scratch;\n"
        "    }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_BORROW_MODE_CONFINED");
    EXPECT_NE(msg.find("borrow"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Vault"), std::string::npos) << msg;
    EXPECT_NE(msg.find("#"), std::string::npos) << msg;
}

// Storing a borrow-mode container into a field lets it outlive the scope of
// what it borrows → compile error at the field declaration.
TEST(BorrowModeConfinementTests, fieldOfBorrowModeContainerIsConfined) {
    std::string src = std::string(kVaultSrc) +
        "public class Registry {\n"
        "    public Vault<Elem> scratch;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_BORROW_MODE_CONFINED");
    EXPECT_NE(msg.find("scratch"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Vault"), std::string::npos) << msg;
}

// Positive control — an OWNING instantiation is a self-contained subtree and
// may live in a field.
TEST(BorrowModeConfinementTests, fieldOfOwningContainerCompiles) {
    std::string src = std::string(kVaultSrc) +
        "public class Registry {\n"
        "    public Vault<#Elem> owned;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Registry r = heap Registry();\n"
        "        r.owned = heap Vault<#Elem>();\n"
        "        Elem e = heap Elem(4);\n"
        "        r.owned.put(#e);\n"
        "        return r.owned.peek().tag;\n"  // 4
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// A `#`-formal takes ownership of its argument; a borrow-mode container
// cannot be owned (§8.2.2 applied at the parameter position).
TEST(BorrowModeConfinementTests, hashFormalOfBorrowModeContainerRejected) {
    std::string src = std::string(kVaultSrc) +
        "public final class D {\n"
        "    public static void take(#Vault<Elem> v) { }\n"
        "    public static int32 run() { return 0; }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_BORROW_MODE_OWNED");
    EXPECT_NE(msg.find("Vault"), std::string::npos) << msg;
}

// ---------------------------------------------------------------------------
// 7.1.3 — you cannot `#`-own a borrow-mode container (§8.2.2): `#` on a type
// argument whose type is itself a borrow-mode instantiation is rejected at
// instantiation, with both fixes named.
// ---------------------------------------------------------------------------

TEST(BorrowModeConfinementTests, owningTypeArgOverBorrowModeContainerRejected) {
    std::string src = std::string(kVaultSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vault<#Vault<Elem>> outer = heap Vault<#Vault<Elem>>();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    std::string msg =
        compileExpectError(src, "CAJETA_ERROR_BORROW_MODE_OWNED");
    EXPECT_NE(msg.find("borrow"), std::string::npos) << msg;
    // Both fixes named: make the inner container owning, or drop the `#`.
    EXPECT_NE(msg.find("#"), std::string::npos) << msg;
}

// Positive control — owning a fully-owning inner container is a
// self-contained subtree and compiles.
TEST(BorrowModeConfinementTests, owningTypeArgOverOwningContainerCompiles) {
    std::string src = std::string(kVaultSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vault<#Vault<#Elem>> outer = heap Vault<#Vault<#Elem>>();\n"
        "        Vault<#Elem> inner = heap Vault<#Elem>();\n"
        "        Elem e = heap Elem(6);\n"
        "        inner.put(#e);\n"
        "        outer.put(#inner);\n"
        "        return outer.peek().peek().tag;\n"  // 6
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}

// Borrowing an owning-mode inner container is fine — the outer container is
// then itself borrow-mode-confined, but building and reading it locally is
// exactly the sanctioned scratch shape (§8.2.1, middle example).
TEST(BorrowModeConfinementTests, borrowingOwningInnerContainerCompiles) {
    std::string src = std::string(kVaultSrc) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vault<#Elem> inner = heap Vault<#Elem>();\n"
        "        Elem e = heap Elem(8);\n"
        "        inner.put(#e);\n"
        "        Vault<Vault<#Elem>> outer = heap Vault<Vault<#Elem>>();\n"
        "        outer.put(inner);\n"
        "        return outer.peek().peek().tag;\n"  // 8
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// ---------------------------------------------------------------------------
// 7.1.2 — an element borrow from a read-only accessor flowed into an OWNING
// position auto-materializes (Unit 3 shared-value path): no error, no dangle.
// ---------------------------------------------------------------------------

TEST(BorrowModeConfinementTests, elementBorrowIntoOwningPositionMaterializes) {
    std::string src =
        "package test;\n"
        "public class Vault<K> {\n"
        "    public K store;\n"
        "    public void put(#K k) { this.store = k; }\n"
        "    public K peek() { return this.store; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vault<String> scratch = heap Vault<String>();\n"
        "        String s = \"hello\";\n"
        "        scratch.put(s);\n"                     // borrow store
        "        String got = scratch.peek();\n"        // element borrow
        "        Vault<#String> owned = heap Vault<#String>();\n"
        "        owned.put(#got);\n"  // borrow → owning position: materializes
        "        return owned.peek().byteLength();\n"   // 5
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// ---------------------------------------------------------------------------
// 7.3.1 (acceptance) — a scratch pipeline over a borrow-mode container leaks
// nothing: the container drops only its shell at scope exit; the borrowed
// elements drop exactly once, with their owners. Overwriting a borrow slot
// must not drop the displaced element (the owner still holds it).
// ---------------------------------------------------------------------------

TEST(BorrowModeConfinementTests, scratchPipelineLeaksNothing) {
    std::string src = std::string(kVaultSrc) +
        "public final class D {\n"
        "    public static int32 work() {\n"
        "        Elem a = heap Elem(1);\n"
        "        Elem b = heap Elem(2);\n"
        "        Vault<Elem> scratch = heap Vault<Elem>();\n"
        "        scratch.put(a);\n"
        "        scratch.put(b);\n"  // displaces the borrow of a — no drop
        "        return scratch.peek().tag;\n"
        "    }\n"
        "    public static int64 run() {\n"
        "        int64 base = Cajeta.liveCount();\n"
        "        int32 v = work();\n"
        "        return (Cajeta.liveCount() - base) * 100 + v;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int64_t (*)()>("run");
    // delta 0 live objects (no leak, no over-drop crash), last-put tag 2.
    EXPECT_EQ(fn(), 2);
}

// ---------------------------------------------------------------------------
// 7.1.4 — a mode-agnostic method template (reads only, never stores or drops
// elements) compiles once and serves BOTH instantiations.
// ---------------------------------------------------------------------------

TEST(BorrowModeConfinementTests, modeAgnosticTemplateServesBothModes) {
    std::string src = std::string(kVaultSrc) +
        "public final class D {\n"
        "    public static T pick<T>(Vault<T> v) { return v.peek(); }\n"
        "    public static int32 run() {\n"
        "        Vault<Elem> scratch = heap Vault<Elem>();\n"
        "        Elem a = heap Elem(3);\n"
        "        scratch.put(a);\n"
        "        Vault<#Elem> owned = heap Vault<#Elem>();\n"
        "        Elem b = heap Elem(4);\n"
        "        owned.put(#b);\n"
        "        return pick(scratch).tag + pick(owned).tag;\n"  // 7
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}
