//
// ModelSelectionHarnessTests.cpp — NET-4.5 model-selection (Server.builder).
//
// Pins the pure-selection contract of the NET-4.5 model-selection surface
// (runtime/src/cajeta/io/net/ServerModel.cajeta +
// runtime/src/cajeta/io/net/ServerBuilder.cajeta), modeled one level down via
// ModelSelectionHarness.h, in the deterministic, scheduler-free posture the
// other net harnesses use while the cajeta-surface accept loop is still being
// wired. The defining NET-4.5 property is that the builder *selects* the right
// accept stack — fiber-per-connection (NET-4.2) or shared-pool-of-N (NET-4.3) —
// with the right pool size and backlog factory, from a `ServerModel` value;
// the live in-scheduler versions of the two stacks are pinned by
// FiberPerConnHarnessTests / SharedPoolHarnessTests. This is the selector that
// chooses between them.
//

#include "gtest/gtest.h"

#include "net/ModelSelectionHarness.h"

#include <stdexcept>

using namespace cajeta::net::testing;

// --- ServerModel factories: kind + pool size --------------------------------
// fiberPerConnection() is Model A (pool size unused); sharedPool(n) is Model B
// carrying n; the kind predicates agree with the kind ordinal.
TEST(ModelSelectionHarnessTests, serverModelFactoriesCarryKindAndPoolSize) {
    ServerModel a = ServerModel::fiberPerConnection();
    EXPECT_EQ(FIBER_PER_CONNECTION_KIND, a.kind);
    EXPECT_TRUE(a.isFiberPerConnection());
    EXPECT_FALSE(a.isSharedPool());

    ServerModel b = ServerModel::sharedPool(8);
    EXPECT_EQ(SHARED_POOL_KIND, b.kind);
    EXPECT_EQ(8, b.poolSize);
    EXPECT_TRUE(b.isSharedPool());
    EXPECT_FALSE(b.isFiberPerConnection());
}

// --- sharedPool floors a non-positive size at 1 -----------------------------
// sharedPool(0) / negative normalizes to a one-worker pool — never zero —
// matching SharedPoolServer's ctor floor.
TEST(ModelSelectionHarnessTests, sharedPoolFloorsSizeAtOne) {
    EXPECT_EQ(1, ServerModel::sharedPool(0).poolSize);
    EXPECT_EQ(1, ServerModel::sharedPool(-4).poolSize);
    EXPECT_EQ(1, ServerModel::sharedPool(1).poolSize);
    EXPECT_EQ(64, ServerModel::sharedPool(64).poolSize);  // a real size passes through
}

// --- the default model is Model A (fiber-per-connection) --------------------
// A builder with no .model(...) step selects fiber-per-connection — the
// simplest, lowest-latency stack, the spec's "Simple, the default."
TEST(ModelSelectionHarnessTests, defaultModelIsFiberPerConnection) {
    BuiltServer s = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .handler()
                        .build();
    EXPECT_EQ(FIBER_PER_CONNECTION_KIND, s.kind) << "no .model(...) ⇒ Model A default";
    EXPECT_EQ(0, s.poolSize) << "Model A carries no pool size";
    EXPECT_FALSE(s.withBacklog) << "no .backlog(...) ⇒ platform default";
}

// --- .model(fiberPerConnection()) selects Model A ---------------------------
TEST(ModelSelectionHarnessTests, fiberPerConnectionModelSelectsModelA) {
    BuiltServer s = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .model(ServerModel::fiberPerConnection())
                        .handler()
                        .build();
    EXPECT_EQ(FIBER_PER_CONNECTION_KIND, s.kind);
    EXPECT_EQ(0, s.poolSize);
}

// --- .model(sharedPool(n)) selects Model B sized to n -----------------------
// The headline selection: a shared-pool model builds a Model-B server with
// exactly the requested worker count threaded through.
TEST(ModelSelectionHarnessTests, sharedPoolModelSelectsModelBWithPoolSize) {
    BuiltServer s = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .model(ServerModel::sharedPool(16))
                        .handler()
                        .build();
    EXPECT_EQ(SHARED_POOL_KIND, s.kind) << "sharedPool(n) ⇒ Model B";
    EXPECT_EQ(16, s.poolSize) << "the requested worker count is threaded to the bind";
}

// --- an explicit .backlog(n) routes through the bindWithBacklog factory -----
// For BOTH models: a set backlog selects the bindWithBacklog factory and
// carries the value; an unset backlog uses the plain bind (platform default).
TEST(ModelSelectionHarnessTests, backlogRoutesThroughBindWithBacklogForBothModels) {
    BuiltServer a = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .model(ServerModel::fiberPerConnection())
                        .backlog(128)
                        .handler()
                        .build();
    EXPECT_EQ(FIBER_PER_CONNECTION_KIND, a.kind);
    EXPECT_TRUE(a.withBacklog);
    EXPECT_EQ(128, a.backlog);

    BuiltServer b = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .model(ServerModel::sharedPool(8))
                        .backlog(256)
                        .handler()
                        .build();
    EXPECT_EQ(SHARED_POOL_KIND, b.kind);
    EXPECT_EQ(8, b.poolSize) << "backlog + pool size are threaded together";
    EXPECT_TRUE(b.withBacklog);
    EXPECT_EQ(256, b.backlog);
}

// --- a sub-1 backlog resets to the platform default -------------------------
TEST(ModelSelectionHarnessTests, nonPositiveBacklogResetsToPlatformDefault) {
    BuiltServer s = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .backlog(0)
                        .handler()
                        .build();
    EXPECT_FALSE(s.withBacklog) << ".backlog(0) ⇒ platform default (unset)";

    BuiltServer t = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .backlog(-5)
                        .handler()
                        .build();
    EXPECT_FALSE(t.withBacklog) << ".backlog(<1) ⇒ platform default (unset)";
}

// --- a null model selection is ignored (the default / prior model stays) ----
// .model(null) must NOT null out the model — the previously-set selection
// (here the default Model A, then an explicit Model B before a null) stays.
TEST(ModelSelectionHarnessTests, nullModelSelectionIsIgnored) {
    // default stays when only a null is passed
    BuiltServer s = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .modelNull()
                        .handler()
                        .build();
    EXPECT_EQ(FIBER_PER_CONNECTION_KIND, s.kind) << "null model ⇒ default Model A unchanged";

    // an earlier explicit model survives a later null
    BuiltServer t = ServerBuilder()
                        .bind("0.0.0.0:8080")
                        .model(ServerModel::sharedPool(4))
                        .modelNull()
                        .handler()
                        .build();
    EXPECT_EQ(SHARED_POOL_KIND, t.kind) << "null model ⇒ prior explicit model unchanged";
    EXPECT_EQ(4, t.poolSize);
}

// --- build with no address is rejected (the required-step guard) ------------
TEST(ModelSelectionHarnessTests, buildWithoutAddressIsRejected) {
    EXPECT_THROW(
        {
            ServerBuilder().model(ServerModel::sharedPool(8)).handler().build();
        },
        std::invalid_argument)
        << "a build with no .bind(addr) must fail the required-step guard";
}

// --- the handler is threaded through the build ------------------------------
TEST(ModelSelectionHarnessTests, handlerIsThreadedThroughBuild) {
    BuiltServer withH = ServerBuilder().bind("0.0.0.0:8080").handler().build();
    EXPECT_TRUE(withH.hasHandler);

    BuiltServer noH = ServerBuilder().bind("0.0.0.0:8080").build();
    EXPECT_FALSE(noH.hasHandler) << "a build records whether a handler was supplied";
}
