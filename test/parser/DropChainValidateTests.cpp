// Tests for the --drop-chain-validate=on runtime hook.
//
// The flag is plumbed via __cajeta_set_drop_chain_validate(int). When on,
// every drop-chain push / pop / mark_inactive checks invariants and aborts
// with a labeled diagnostic on corruption. Per CompilerModes.md, this is
// a debug-only feature — release builds skip the checks entirely.
//
// Death tests exercise the abort path; happy-path tests confirm the
// instrumented sequence behaves identically to the uninstrumented one.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../DeathTestUtil.h"

#include <cstdint>

using cajeta_test::CajetaJit;

// Runtime entry points exported from cajeta_runtime.c. Layout of
// cajeta_drop_entry is duplicated here verbatim — the runtime struct
// is internal, but its first three pointer fields plus an int8 active
// flag are the load-bearing ABI shape used by the IR side too.
extern "C" {
    struct cajeta_drop_entry_layout {
        void* obj;
        void (*drop_fn)(void*);
        struct cajeta_drop_entry_layout* prev;
        int8_t active;
    };

    void __cajeta_set_drop_chain_validate(int enabled);
    int  __cajeta_get_drop_chain_validate(void);
    void __cajeta_set_poison_free(int enabled);
    void __cajeta_drop_push(void* e, void* obj, void (*drop_fn)(void*));
    void __cajeta_drop_pop_run(void* e);
    void __cajeta_drop_mark_inactive(void* e);
}

namespace {

// No-op drop function used as a safe payload for chain pushes.
extern "C" void noopDropFn(void* /*unused*/) {}

class DropValidateStateGuard {
public:
    DropValidateStateGuard() : saved(__cajeta_get_drop_chain_validate()) {}
    ~DropValidateStateGuard() { __cajeta_set_drop_chain_validate(saved); }
private:
    int saved;
};

} // namespace

// Setter round-trip.
TEST(DropChainValidateTests, setterRoundTrip) {
    DropValidateStateGuard guard;
    __cajeta_set_drop_chain_validate(0);
    EXPECT_EQ(__cajeta_get_drop_chain_validate(), 0);
    __cajeta_set_drop_chain_validate(1);
    EXPECT_EQ(__cajeta_get_drop_chain_validate(), 1);
    __cajeta_set_drop_chain_validate(7);  // truthy normalizes to 1
    EXPECT_EQ(__cajeta_get_drop_chain_validate(), 1);
    __cajeta_set_drop_chain_validate(0);
    EXPECT_EQ(__cajeta_get_drop_chain_validate(), 0);
}

// Happy path: well-ordered push/pop sequence runs cleanly with validation
// enabled. Pushes two entries LIFO, pops them in reverse order; no abort.
TEST(DropChainValidateTests, lifoSequencePassesValidation) {
    DropValidateStateGuard guard;
    __cajeta_set_drop_chain_validate(1);

    int countdownPayload = 0;
    cajeta_drop_entry_layout a{};
    cajeta_drop_entry_layout b{};
    __cajeta_drop_push(&a, &countdownPayload, noopDropFn);
    __cajeta_drop_push(&b, &countdownPayload, noopDropFn);
    // LIFO pop: b first, then a.
    __cajeta_drop_pop_run(&b);
    __cajeta_drop_pop_run(&a);
    SUCCEED();
}

// Validation off → out-of-order pop is silently tolerated (pre-flag
// behavior). The chain's top pointer winds up corrupt afterward, but no
// abort fires. Pop both entries cleanly to restore the chain before the
// test exits.
TEST(DropChainValidateTests, outOfOrderPopSilentWhenDisabled) {
    DropValidateStateGuard guard;
    __cajeta_set_drop_chain_validate(0);

    int payload = 0;
    cajeta_drop_entry_layout a{};
    cajeta_drop_entry_layout b{};
    __cajeta_drop_push(&a, &payload, noopDropFn);
    __cajeta_drop_push(&b, &payload, noopDropFn);

    // Out-of-order: pop A while B is on top. With validation off, this
    // succeeds and leaves *top = a.prev (which is the chain's previous
    // state). The b entry is dangling — only safe because we drop the
    // whole chain right after.
    __cajeta_drop_pop_run(&a);
    SUCCEED();
}

// Death test: pop with mismatched top. Push A, push B, pop A → top is B,
// not A → CAJETA_ERROR_DROP_CHAIN_POP_MISMATCH.
TEST(DropChainValidateDeathTests, popMismatchAborts) {
    ASSERT_EXIT({
        __cajeta_set_drop_chain_validate(1);
        int payload = 0;
        cajeta_drop_entry_layout a{};
        cajeta_drop_entry_layout b{};
        __cajeta_drop_push(&a, &payload, noopDropFn);
        __cajeta_drop_push(&b, &payload, noopDropFn);
        __cajeta_drop_pop_run(&a);  // mismatch — top is &b
    }, CAJETA_DIED_BY_ABORT,
       "CAJETA_ERROR_DROP_CHAIN_POP_MISMATCH");
}

// Death test: push with NULL entry. Validation rejects before the
// would-be NULL dereference inside the body.
TEST(DropChainValidateDeathTests, nullPushAborts) {
    ASSERT_EXIT({
        __cajeta_set_drop_chain_validate(1);
        int payload = 0;
        __cajeta_drop_push(nullptr, &payload, noopDropFn);
    }, CAJETA_DIED_BY_ABORT,
       "CAJETA_ERROR_DROP_CHAIN_NULL_PUSH");
}

// Death test: pop with NULL entry. Same shape as null push; validation
// catches it before the body deref.
TEST(DropChainValidateDeathTests, nullPopAborts) {
    ASSERT_EXIT({
        __cajeta_set_drop_chain_validate(1);
        __cajeta_drop_pop_run(nullptr);
    }, CAJETA_DIED_BY_ABORT,
       "CAJETA_ERROR_DROP_CHAIN_NULL_POP");
}

// Death test: mark_inactive on NULL. Same NULL-safety story.
TEST(DropChainValidateDeathTests, nullMarkInactiveAborts) {
    ASSERT_EXIT({
        __cajeta_set_drop_chain_validate(1);
        __cajeta_drop_mark_inactive(nullptr);
    }, CAJETA_DIED_BY_ABORT,
       "CAJETA_ERROR_DROP_CHAIN_NULL_MARK");
}

// Death test: self-push. Pushing an entry whose pointer equals the
// current top would create a self-cycle on `prev`. Push A, then push A
// again — the second push sees *top == &a.
TEST(DropChainValidateDeathTests, selfPushAborts) {
    ASSERT_EXIT({
        __cajeta_set_drop_chain_validate(1);
        int payload = 0;
        cajeta_drop_entry_layout a{};
        __cajeta_drop_push(&a, &payload, noopDropFn);
        __cajeta_drop_push(&a, &payload, noopDropFn);  // self-cycle
    }, CAJETA_DIED_BY_ABORT,
       "CAJETA_ERROR_DROP_CHAIN_SELF_PUSH");
}

// Death test: bit-rotted active flag at pop time. Push an entry, scribble
// 0x7F into active (neither 0 nor 1), then try to pop — validation
// catches the bit-rot before the dispatch on active.
TEST(DropChainValidateDeathTests, badActiveOnPopAborts) {
    ASSERT_EXIT({
        __cajeta_set_drop_chain_validate(1);
        int payload = 0;
        cajeta_drop_entry_layout a{};
        __cajeta_drop_push(&a, &payload, noopDropFn);
        a.active = 0x7F;  // bit-rot
        __cajeta_drop_pop_run(&a);
    }, CAJETA_DIED_BY_ABORT,
       "CAJETA_ERROR_DROP_CHAIN_BAD_ACTIVE");
}

// JIT integration: a normal cajeta program with drop-chain-validate on
// runs without spurious aborts. The compiler's emit shape obeys the
// LIFO discipline by construction; this pins that the validation
// doesn't false-positive against well-formed user code.
TEST(DropChainValidateTests, jitProgramRunsCleanlyWithValidationOn) {
    auto src =
        "package test;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 x) { this.v = x; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Box b = heap Box(3);\n"
        "        Box c = heap Box(4);\n"
        "        return b.v + c.v;\n"
        "    }\n"
        "}\n";
    CajetaJit::Options opts;
    opts.dropChainValidateEnabled = true;
    auto jit = CajetaJit::compile(src, "test.D", opts);
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
    EXPECT_EQ(__cajeta_get_drop_chain_validate(), 1);
}
