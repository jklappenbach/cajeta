//
// cajeta.concurrent.Channel<T> (R7-F) — bounded MPMC ring-buffer queue on
// the lock+condvar intrinsics. send blocks while full; receive returns a
// stack Optional<T> (present while items remain, empty once closed+drained).
// Loaded from the embedded stdlib (runtime/src/cajeta/concurrent/Channel.cajeta).
//
// Main-thread tests stay within capacity / drain only after close so they
// never block (no producer to wake them). The fiber test exercises the
// full/empty blocking hand-off under the single cooperative carrier.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string chanTestSource(const std::string& dBody) {
    return std::string("package test;\n")
        + "import cajeta.concurrent.Channel;\n"
        + "import cajeta.lang.Optional;\n"
        + "public final class D {\n" + dBody + "}\n";
}

int32_t runI32(const std::string& dBody) {
    auto jit = CajetaJit::compile(chanTestSource(dBody), "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace


// Single send then receive (buffer non-empty on receive -> no block).

// receive() on a closed + empty channel returns an empty Optional.

// Buffered items drain after close, then the channel reads empty.

// Bounded producer/consumer across fibers: capacity 2, producer sends 3
// (blocks on the 3rd until the consumer drains one), closes; consumer drains
// via the Optional loop until empty. Exercises full/empty blocking + close.

// 2P + 2C — the actual MPMC contract. Two producers each send three
// distinct items (10,20,30 and 100,200,300; total 660). Two consumers
// drain in parallel via the Optional loop, accumulating into disjoint
// slots so we get a stable per-consumer partial then sum on readback.
// Total must match irrespective of which consumer wins which item — the
// only invariant being checked at this layer is "every send is received
// exactly once". Capacity 2 keeps the buffer tight so producer/consumer
// hand-off blocking exercises both `full` (producer-side) and `empty`
// (consumer-side) condvar waits.
//
// Sequencing: an inner scope owns the two producer spawns and waits for
// both to finish before falling through to ch.close(); the outer scope
// owns the two consumer spawns so they're already running while the
// producers fill the channel, and the outer scope's exit waits on both
// consumers to drain post-close. Without the nested scope the closer
// would race the producers and one `send` would land on a closed channel
// (throws 1).

// receive() returns the canonical Optional<T>; this test pins that
// orElse() works on the closed-and-drained path.

// Three sequential `Optional<int32>` locals from receive() coexist in the
// same frame; each owns its own sret slot and the values don't alias.
// Uses isPresent guards rather than orElse so the test is a pure
// stack-Optional-locals stress, not orElse coverage (orElse on
// sret-return Optional<T> still trips up — tracked separately).
TEST(ChannelTests, sequentialReceivesIsPresent) {
    EXPECT_EQ(runI32(
        "    public static int32 run() {\n"
        "        Channel<int32> ch = heap Channel<int32>(2);\n"
        "        ch.send(11);\n"
        "        ch.send(22);\n"
        "        ch.close();\n"
        "        Optional<int32> oa = ch.receive();\n"
        "        int32 a = oa.get();\n"
        "        Optional<int32> ob = ch.receive();\n"
        "        int32 b = ob.get();\n"
        "        Optional<int32> oc = ch.receive();\n"
        "        int32 c = -1;\n"
        "        if (oc.isPresent()) { c = oc.get(); }\n"
        "        return a + b + c;\n"
        "    }\n"
    ), 32);
}
