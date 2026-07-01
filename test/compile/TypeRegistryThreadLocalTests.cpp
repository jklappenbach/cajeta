#include <gtest/gtest.h>
#include <thread>
#include "cajeta/type/CajetaType.h"

using namespace cajeta;

// Unit 2 (thread-safe compiler): the CajetaType type registries are thread_local,
// so a mutation on one thread is invisible to another — independent compiles on
// different threads never share type state. Probed via the public enum-constant
// registry (the registries themselves are protected).
TEST(TypeRegistryThreadLocalTests, enumRegistryIsThreadLocal) {
    CajetaType::registerEnumConstant("TLProbeMain", "X", 7);
    ASSERT_TRUE(CajetaType::isEnumName("TLProbeMain"));

    bool workerSawMain = true;
    std::thread t([&] {
        workerSawMain = CajetaType::isEnumName("TLProbeMain");      // expect false
        CajetaType::registerEnumConstant("TLProbeWorker", "Y", 9);  // worker-only
    });
    t.join();

    EXPECT_FALSE(workerSawMain) << "main-thread registration leaked into worker";
    EXPECT_FALSE(CajetaType::isEnumName("TLProbeWorker"))
        << "worker-thread registration leaked into main";
    EXPECT_TRUE(CajetaType::isEnumName("TLProbeMain")); // main still sees its own
}
