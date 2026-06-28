#include <gtest/gtest.h>
#include <thread>
#include "cajeta/compile/CajetaModule.h"

using namespace cajeta;

// Unit 3 (thread-safe compiler): the per-compile module/method registries are
// thread_local, so compile state set on one thread is invisible to another.
// Probed via the public activeProfile accessors (default "prod").
TEST(ModuleRegistryThreadLocalTests, activeProfileIsThreadLocal) {
    CajetaModule::setActiveProfile("test");
    ASSERT_EQ(CajetaModule::getActiveProfile(), "test");

    std::string workerSaw;
    std::thread t([&] {
        workerSaw = CajetaModule::getActiveProfile();   // expect "prod" (own default)
        CajetaModule::setActiveProfile("worker");        // worker-only
    });
    t.join();

    EXPECT_EQ(workerSaw, "prod") << "main-thread activeProfile leaked into worker";
    EXPECT_EQ(CajetaModule::getActiveProfile(), "test")
        << "worker-thread activeProfile leaked into main";
}
