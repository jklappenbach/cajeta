//
// Channel.select (R9.6) — Tasks.selectReceive<T> multiplexed receive across
// an array of Channel<T>. v1 implementation polls each channel via
// tryReceive() with exponential-backoff fiberSleepNanos between passes.
// Returns `Optional<SelectResult<T>>`: present wraps a SelectResult carrying
// `index` (the channel that fired) and `value` (the dequeued item); empty
// once every channel is closed AND drained.
//
// Tests exercise: (a) lowest-index-wins when multiple channels are ready
// in the same pass; (b) producer publishes on one channel only, select
// blocks until that channel fires; (c) all-closed-and-drained terminal
// (empty Optional).
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

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

// Two channels pre-loaded with one item each. Both ready in the very first
// poll pass; selectReceive returns a present Optional with index 0
// (lowest wins) and its value. Encodes (index, value) as 1000 * (index+1) +
// value so a single i32 covers both fields end-to-end.
TEST(ChannelSelectTests, lowestIndexWinsOnSimultaneousReady) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.Channel;\n"
        "import cajeta.threading.Tasks;\n"
        "import cajeta.threading.SelectResult;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Channel<int32> a = new Channel<int32>(2);\n"
        "        Channel<int32> b = new Channel<int32>(2);\n"
        "        a.send(7);\n"
        "        b.send(99);\n"
        "        Channel<int32>[] chs = new Channel<int32>[2];\n"
        "        chs[0] = a;\n"
        "        chs[1] = b;\n"
        "        Optional<SelectResult<int32>> opt = Tasks.selectReceive<int32>(chs);\n"
        "        a.close();\n"
        "        b.close();\n"
        "        if (!opt.isPresent()) { return -1; }\n"
        "        SelectResult<int32> r = opt.get();\n"
        "        return 1000 * (r.index + 1) + r.value;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1007);
}

// Producer publishes on channel B only; selectReceive on both should
// block (poll loop) until B's value arrives, then return present with
// index=1. Exercises the multi-pass / backoff path.
TEST(ChannelSelectTests, producerOnOneChannel) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.Channel;\n"
        "import cajeta.threading.Tasks;\n"
        "import cajeta.threading.SelectResult;\n"
        "public final class D {\n"
        "    public static async int32 publishOnB(Channel<int32> b) {\n"
        "        b.send(42);\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Channel<int32> a = new Channel<int32>(2);\n"
        "        Channel<int32> b = new Channel<int32>(2);\n"
        "        Channel<int32>[] chs = new Channel<int32>[2];\n"
        "        chs[0] = a;\n"
        "        chs[1] = b;\n"
        "        int32 idx = -2;\n"
        "        int32 val = -2;\n"
        "        scope {\n"
        "            spawn publishOnB(b);\n"
        "            Optional<SelectResult<int32>> opt = Tasks.selectReceive<int32>(chs);\n"
        "            if (opt.isPresent()) {\n"
        "                SelectResult<int32> r = opt.get();\n"
        "                idx = r.index;\n"
        "                val = r.value;\n"
        "            }\n"
        "        }\n"
        "        a.close();\n"
        "        b.close();\n"
        "        return 1000 * (idx + 1) + val;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2042);
}

// All channels closed and drained before the select fires; selectReceive
// returns an empty Optional. Encode as 9999 to distinguish.
TEST(ChannelSelectTests, allClosedReturnsEmpty) {
    auto src =
        "package test;\n"
        "import cajeta.lang.Optional;\n"
        "import cajeta.threading.Channel;\n"
        "import cajeta.threading.Tasks;\n"
        "import cajeta.threading.SelectResult;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Channel<int32> a = new Channel<int32>(1);\n"
        "        Channel<int32> b = new Channel<int32>(1);\n"
        "        a.close();\n"
        "        b.close();\n"
        "        Channel<int32>[] chs = new Channel<int32>[2];\n"
        "        chs[0] = a;\n"
        "        chs[1] = b;\n"
        "        Optional<SelectResult<int32>> opt = Tasks.selectReceive<int32>(chs);\n"
        "        if (!opt.isPresent()) { return 9999; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9999);
}
