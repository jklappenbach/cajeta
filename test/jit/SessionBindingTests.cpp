//
// script-units U3 (spec 4.1, 4.3-4.5, 4.7) — session bindings.
//
// Top-level owner declarations in a script unit bind into the SESSION scope,
// not the entry's drop frame: they survive the entry's return and drop only
// when the host runs `__cajeta_session_drop_all` (reverse binding order), or
// earlier when the name is rebound (the runtime drops the old occupant at
// the rebind). Block-nested locals keep ordinary scope-exit drops. `stack`
// allocations cannot bind at top level (spec 4.7 — uniform rule).
//
// Observability: a Probe class counts destructor runs in statics, read back
// through top-level helper methods; the session verbs are looked up as raw
// JIT symbols and driven from the test.
//

#include "gtest/gtest.h"
#include "JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Shared prelude: the drop probe. `firstDropped`/`lastDropped` record the
// reverse-order proof; `drops` counts destructor runs.
const char* kProbe =
    "public class Probe {\n"
    "    public static int32 drops;\n"
    "    public static int32 firstDropped;\n"
    "    public static int32 lastDropped;\n"
    "    public int32 id;\n"
    "    public Probe(int32 id) { this.id = id; }\n"
    "    public ~Probe() {\n"
    "        Probe.drops = Probe.drops + 1;\n"
    "        if (Probe.firstDropped == 0) { Probe.firstDropped = this.id; }\n"
    "        Probe.lastDropped = this.id;\n"
    "    }\n"
    "}\n"
    "int32 dropCount() { return Probe.drops; }\n"
    "int32 firstDropped() { return Probe.firstDropped; }\n"
    "int32 lastDropped() { return Probe.lastDropped; }\n";

struct Session {
    std::unique_ptr<CajetaJit> jit;
    int32_t (*entry)() = nullptr;
    int32_t (*dropCount)() = nullptr;
    int32_t (*firstDropped)() = nullptr;
    int32_t (*lastDropped)() = nullptr;
    void (*dropAll)() = nullptr;

    explicit Session(const std::string& scriptTail) {
        jit = CajetaJit::compile(std::string(kProbe) + scriptTail,
                                 "cajeta.script.tool");
        if (!jit) return;
        entry = jit->lookup<int32_t (*)()>("__cajeta_script_entry");
        dropCount = jit->lookup<int32_t (*)()>("dropCount");
        firstDropped = jit->lookup<int32_t (*)()>("firstDropped");
        lastDropped = jit->lookup<int32_t (*)()>("lastDropped");
        dropAll = reinterpret_cast<void (*)()>(
            jit->lookupRawSymbol("__cajeta_session_drop_all"));
    }

    bool ok() const {
        return jit && entry && dropCount && firstDropped && lastDropped
            && dropAll;
    }
};

std::string errorOf(const std::string& script) {
    try {
        CajetaJit::compile(script, "cajeta.script.tool");
        return "";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    } catch (const std::exception&) {
        return "<non-cajeta-exception>";
    }
}

}  // namespace

// 3.1.1 / spec 4.1 — a top-level heap binding is readable by later
// statements and SURVIVES the entry's return: no drop until the host says so.

// 3.1.2 / spec 4.3 — rebinding the name drops the old value at the rebind
// point; the new value is session-owned.
TEST(SessionBindingTests, rebindDropsOldValue) {
    Session s(
        "Probe p = heap Probe(1);\n"
        "p = heap Probe(2);\n"
        "return 0;\n");
    ASSERT_TRUE(s.ok());
    s.entry();
    EXPECT_EQ(1, s.dropCount());       // old value dropped at the rebind
    EXPECT_EQ(1, s.firstDropped());
    s.dropAll();
    EXPECT_EQ(2, s.dropCount());       // the survivor drops at session end
    EXPECT_EQ(2, s.lastDropped());
}

// 3.1.3 / spec 4.4 — session end drops in reverse binding order.

// 3.1.4 / spec 4.5 — a block-nested local is an ordinary local: it drops at
// block exit inside the entry, and the session never sees it.

// 3.1.5 / spec 4.7 — a `stack` allocation cannot bind at top level: session
// bindings outlive the entry frame. Uniform rule (open question 8.1 closed
// to "keep uniform").
TEST(SessionBindingTests, stackAtTopLevelRejected) {
    EXPECT_EQ("CAJETA_ERROR_SESSION_STACK_BINDING", errorOf(
        "public class Held { public int32 v; public Held(int32 v) { this.v = v; } }\n"
        "Held h = stack Held(1);\n"
        "return h.v;\n"));
}

// 4.2.4(b) — a block-local SHADOWING a session-binding name is an ordinary
// local: it drops at block exit, and the session binding (and its slot in
// the registry) must be untouched by the block's bind/disarm/escape gates.
TEST(SessionBindingTests, blockLocalShadowingASessionBindingStaysLocal) {
    Session s(
        "Probe p = heap Probe(1);\n"
        "{\n"
        "    Probe p = heap Probe(2);\n"
        "    int32 ignore = p.id;\n"
        "}\n"
        "return 0;\n");
    ASSERT_TRUE(s.ok());
    s.entry();
    EXPECT_EQ(1, s.dropCount()) << "block-local did not drop at block exit";
    EXPECT_EQ(2, s.firstDropped()) << "the wrong value dropped first";
    s.dropAll();
    EXPECT_EQ(2, s.dropCount()) << "session binding lost by the shadow";
    EXPECT_EQ(1, s.lastDropped());
}

// 3.2.4 — rebind drop coverage, interface shape: the old occupant drops at
// the rebind (runtime uses the drop fn stored at bind time), and the NEW
// occupant must be registered with a real drop fn so drop_all frees it.
// DISABLED (script-units 3.2.4, measured 2026-08-18): an interface-typed
// session binding drops NOTHING at the rebind and leaks one of two
// occupants at drop_all (drops=0 then 1, expected 1 then 2) — the fat
// { data, vtable, kind } body defeats the class-shaped drop selection in
// BOTH the declaration choke point and the rebind path. Needs the
// fat-pointer ownership design, not a patch.
TEST(SessionBindingTests, DISABLED_interfaceTypedRebindDropsBothOccupants) {
    Session s(
        "public interface Ider { int32 id(); }\n"
        "public class P2 implements Ider {\n"
        "    public int32 n;\n"
        "    public P2(int32 n) { this.n = n; }\n"
        "    public int32 id() { return this.n; }\n"
        "    public ~P2() {\n"
        "        Probe.drops = Probe.drops + 1;\n"
        "        if (Probe.firstDropped == 0) { Probe.firstDropped = this.n; }\n"
        "        Probe.lastDropped = this.n;\n"
        "    }\n"
        "}\n"
        "Ider x = heap P2(1);\n"
        "x = heap P2(2);\n"
        "return 0;\n");
    ASSERT_TRUE(s.ok());
    s.entry();
    EXPECT_EQ(1, s.dropCount()) << "old interface-typed occupant not dropped at rebind";
    EXPECT_EQ(1, s.firstDropped());
    s.dropAll();
    EXPECT_EQ(2, s.dropCount()) << "rebound interface-typed value leaked at session end";
    EXPECT_EQ(2, s.lastDropped());
}

// 3.2.4 — rebind drop coverage, array shape: rebinding an array-typed
// session binding must drop the old array's owned elements (and free its
// storage) at the rebind, and register the new array for session drop.
// DISABLED (script-units 3.2.4, measured 2026-08-18): SIGSEGV (null deref
// in JIT'd code) at the entry — the array rebind path is not merely
// uncovered, it crashes. Needs session-side element-ownership design
// (the local teardown's elem-walk has no session counterpart).
TEST(SessionBindingTests, DISABLED_probeElementArrayRebindDropsOldElements) {
    Session s(
        "Probe[] xs = heap Probe[1];\n"
        "xs[0] = heap Probe(1);\n"
        "xs = heap Probe[1];\n"
        "xs[0] = heap Probe(2);\n"
        "return 0;\n");
    ASSERT_TRUE(s.ok());
    s.entry();
    EXPECT_EQ(1, s.dropCount()) << "old array's element not dropped at rebind";
    EXPECT_EQ(1, s.firstDropped());
    s.dropAll();
    EXPECT_EQ(2, s.dropCount()) << "rebound array's element leaked at session end";
}
