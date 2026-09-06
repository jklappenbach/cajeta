// Optional relay return ABI.
//
// A method's by-value (sret) return ABI used to be decided by a SYNTACTIC scan
// of its body for a literal `return stack X(...)`. A method that RELAYS a
// stack Optional it obtained from a call — `Optional<int32> take() { return
// this.ch.receive(); }` — has no `stack` in its body, so it was typed as a
// pointer-returning function while its body `ret`s the struct value the
// callee produced through sret. LLVM's verifier rejects that IR ("Function
// return type does not match operand type of return inst"); where nothing
// stopped it, the caller read the struct bits as a pointer and the Optional
// arrived empty (the cabra LineReader defect, 2026-08-31; the KernelManifest
// accessors, 2026-09-06 — nine relays, printed as nine failures under the
// merged module's isomorphic type names).
//
// The rule is now decided by the RETURN TYPE for the value-shape class:
// `cajeta.lang.Optional<T>` returns are always sret, whatever the body does
// (the interface-decl path already said so, #63). The body scan still turns on
// sret for any other class returned by `return stack X(...)`.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <map>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kHelpers =
    "    private static Optional<int32> opt(int32 v) {\n"
    "        if (v < 0) { return stack Optional<int32>(false, 0); }\n"
    "        return stack Optional<int32>(true, v);\n"
    "    }\n"
    "    private static Optional<String> optStr(String s) {\n"
    "        if (s == null) { return stack Optional<String>(false); }\n"
    "        return stack Optional<String>(true, s);\n"
    "    }\n";

std::string wrap(const std::string& body) {
    return "package test;\n"
           "import cajeta.lang.Optional;\n"
           "public final class S {\n"
         + body +
           "}\n";
}

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.S");
    if (!jit) return -100;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -101;
    return fn();
}

// The error id, or "" when the source compiles.
std::string errorOf(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.S");
        return "";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    } catch (const std::exception&) {
        return "<non-cajeta-exception>";
    }
}

std::string irOf(const std::string& src) {
    CajetaJit::Options o;
    o.captureIr = true;
    auto jit = CajetaJit::compile(src, "test.S", o);
    return jit ? jit->getModuleIr() : std::string("<no jit>");
}

} // namespace

// A one-line relay through a static helper: present flag AND value survive.
TEST(OptionalRelayReturn, relayThroughStaticHelperKeepsPresentFlagAndValue) {
    auto src = wrap(std::string(kHelpers) +
        "    public static Optional<int32> relay(int32 v) { return S.opt(v); }\n"
        "    public static Optional<String> relayStr(String s) { return S.optStr(s); }\n"
        "    public static int32 run() {\n"
        "        Optional<int32> a = S.relay(7);\n"
        "        Optional<int32> b = S.relay(-1);\n"
        "        Optional<String> c = S.relayStr(\"x\");\n"
        "        Optional<String> d = S.relayStr(null);\n"
        "        if (!a.isPresent()) { return 1; }\n"
        "        if (a.get() != 7) { return 2; }\n"
        "        if (b.isPresent()) { return 3; }\n"
        "        if (!c.isPresent()) { return 4; }\n"
        "        if (!c.get().equals(\"x\")) { return 5; }\n"
        "        if (d.isPresent()) { return 6; }\n"
        "        return 0;\n"
        "    }\n");
    EXPECT_EQ(runI32(src), 0);
}

// The cabra shape: an instance method relaying a generic's Optional producer.
TEST(OptionalRelayReturn, relayThroughInstanceMethodOfGeneric) {
    std::map<std::string, std::string> sources;
    sources["test.G"] =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public class G<T> {\n"
        "    T item;\n"
        "    boolean has;\n"
        "    public G(boolean has, T item) { this.has = has; this.item #= item; }\n"
        "    public Optional<T> find() {\n"
        "        if (this.has) { return stack Optional<T>(true, this.item); }\n"
        "        return stack Optional<T>(false);\n"
        "    }\n"
        "}\n";
    sources["test.S"] =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "public final class S {\n"
        "    G<String> gs;\n"
        "    G<int32> gi;\n"
        "    public S() {\n"
        "        this.gs #= heap G<String>(true, \"x\");\n"
        "        this.gi #= heap G<int32>(false, 0);\n"
        "    }\n"
        "    public Optional<String> takeStr() { return this.gs.find(); }\n"
        "    public Optional<int32> takeInt() { return this.gi.find(); }\n"
        "    public static int32 run() {\n"
        "        S s = heap S();\n"
        "        Optional<String> a = s.takeStr();\n"
        "        Optional<int32> b = s.takeInt();\n"
        "        if (!a.isPresent()) { return 1; }\n"
        "        if (!a.get().equals(\"x\")) { return 2; }\n"
        "        if (b.isPresent()) { return 3; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(sources, "test.S");
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// A relay two hops deep, and a relay whose value is consumed through orElse.
TEST(OptionalRelayReturn, doubleRelayAndOrElse) {
    auto src = wrap(std::string(kHelpers) +
        "    public static Optional<int32> relay(int32 v) { return S.opt(v); }\n"
        "    public static Optional<int32> relay2(int32 v) { return S.relay(v); }\n"
        "    public static int32 run() {\n"
        "        int32 a = S.relay2(11).orElse(-1);\n"
        "        int32 b = S.relay2(-5).orElse(-1);\n"
        "        if (a != 11) { return 1; }\n"
        "        if (b != -1) { return 2; }\n"
        "        return 0;\n"
        "    }\n");
    EXPECT_EQ(runI32(src), 0);
}

// The IR: the relay is lowered with the sret ABI like the producer it relays;
// no Optional-returning function is declared `define ptr`.
TEST(OptionalRelayReturn, relayIsLoweredWithSret) {
    auto src = wrap(std::string(kHelpers) +
        "    public static Optional<int32> relay(int32 v) { return S.opt(v); }\n"
        "    public static int32 run() { return S.relay(1).orElse(0); }\n");
    std::string ir = irOf(src);
    EXPECT_NE(ir.find("define void @\"test.S::relay(v:int32)\"(ptr sret("),
              std::string::npos) << ir.substr(0, 4000);
    EXPECT_NE(ir.find("define void @\"test.S::opt(v:int32)\"(ptr sret("),
              std::string::npos);
    EXPECT_EQ(ir.find("define ptr @\"test.S::relay("), std::string::npos);
}

// `return null` in a by-value Optional method has nothing to copy into the
// caller's slot: a compile error naming the fix, not a memcpy from address 0.
TEST(OptionalRelayReturn, nullReturnFromByValueOptionalIsRejected) {
    auto src = wrap(
        "    public static Optional<int32> bad() { return null; }\n"
        "    public static int32 run() { return S.bad().orElse(0); }\n");
    EXPECT_EQ(errorOf(src), "CAJETA_ERROR_NULL_RETURN_BY_VALUE");
}

// `return heap Optional<T>(...)` from a plain (by-value) Optional method: the
// heap object would be copied out and leaked. Rejected with the fix spelled.
TEST(OptionalRelayReturn, heapReturnFromByValueOptionalIsRejected) {
    auto src = wrap(
        "    public static Optional<int32> bad() { return heap Optional<int32>(true, 1); }\n"
        "    public static int32 run() { return S.bad().orElse(0); }\n");
    EXPECT_EQ(errorOf(src), "CAJETA_ERROR_HEAP_RETURN_BY_VALUE");
}

// An owned `#Optional<T>` return is the heap form and is untouched by the rule.
TEST(OptionalRelayReturn, ownedHeapOptionalReturnStillCompiles) {
    auto src = wrap(
        "    public static #Optional<int32> owned(int32 v) { return heap Optional<int32>(true, v); }\n"
        "    public static int32 run() {\n"
        "        Optional<int32> o #= S.owned(9);\n"
        "        return o.isPresent() && o.get() == 9 ? 0 : 1;\n"
        "    }\n");
    EXPECT_EQ(errorOf(src), "");
    EXPECT_EQ(runI32(src), 0);
}

// A lambda with an INFERRED type whose expression body relays an sret call:
// the function type must take the sret form from the return type, as a
// method does.
TEST(OptionalRelayReturn, lambdaRelayKeepsPresentFlag) {
    auto src = wrap(std::string(kHelpers) +
        "    public static int32 run() {\n"
        "        (int32) -> Optional<int32> f = (int32 v) -> S.opt(v);\n"
        "        Optional<int32> a = f(7);\n"
        "        Optional<int32> b = f(-1);\n"
        "        if (!a.isPresent()) { return 1; }\n"
        "        if (a.get() != 7) { return 2; }\n"
        "        if (b.isPresent()) { return 3; }\n"
        "        return 0;\n"
        "    }\n");
    EXPECT_EQ(runI32(src), 0);
}
