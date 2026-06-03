//
// ModelSelectionHarness.h — NET-4.5 model-selection (Server.builder) harness.
//
// NET-4.2 (FiberPerConnHarness.h) and NET-4.3 (SharedPoolHarness.h) pin the
// two accept *models* — fiber-per-connection and event-driven shared-pool.
// NET-4.5 is the **model-selection surface** that sits in front of them: the
// `Server.builder()` fluent API + the `ServerModel` value that names which of
// the two shipped accept stacks a build wires up
// (runtime/src/cajeta/net/ServerModel.cajeta +
// runtime/src/cajeta/net/ServerBuilder.cajeta).
//
// Its distinct, testable contract — over and above the two models the
// dependency harnesses already freeze — is pure *selection* logic, no I/O:
//
//   1. **The default is Model A.** A builder with no `.model(...)` step
//      selects fiber-per-connection (the simplest, lowest-latency stack).
//
//   2. **`.model(fiberPerConnection())` selects Model A; `.model(sharedPool(n))`
//      selects Model B sized to `n`.** The kind the builder records, and the
//      pool size it threads, must match the chosen model exactly.
//
//   3. **A shared-pool size is floored at 1.** `sharedPool(0)` / a negative
//      request normalizes up to a one-worker pool — never a zero-worker pool —
//      the same floor SharedPoolServer's ctor applies.
//
//   4. **The backlog knob is threaded to the right factory.** An explicit
//      `.backlog(n)` routes through the `bindWithBacklog` factory of whichever
//      model is selected; an unset (`< 1`) backlog routes through the plain
//      `bind` and leaves the platform default.
//
//   5. **A `null` model selection is ignored** — the previously-set model (the
//      default, or an earlier `.model`) stays in place; `.model(...)` can
//      never null out the model.
//
//   6. **A build with no address is rejected** (the required-step guard).
//
// ## Why a C++ harness (not a `.cajeta` one)
//
// Identical rationale to the NET-4.1/4.2/4.3 harnesses: the cajeta-surface
// socket lowering the *full* accept loop needs is still being wired, so the
// live `serve()` cannot be JIT-run deterministically yet. But the
// model-*selection* logic these sources encode — which concrete server
// (Model A vs Model B), with which pool size and which backlog factory, a
// given builder configuration produces — is platform-independent, socket-free,
// and deterministically pinnable on its own. That is exactly what this header
// models, mirroring ServerModel.{fiberPerConnection,sharedPool} +
// ServerBuilder.build() one level down: no real sockets, just a record of the
// factory call the build *would* make. The selection semantics here MUST
// mirror those two Cajeta sources exactly — they are the executable spec for
// that pure-logic selector. Kept under test/ so the production sources carry
// no test-only surface.
//
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace cajeta::net::testing {

    // -----------------------------------------------------------------------
    // ServerModelKind — mirrors ServerModel.{FIBER_PER_CONNECTION_KIND,
    // SHARED_POOL_KIND}. Append-only ordinals (never renumber).
    // -----------------------------------------------------------------------
    enum ServerModelKind : int32_t {
        FIBER_PER_CONNECTION_KIND = 0,
        SHARED_POOL_KIND          = 1,
    };

    // -----------------------------------------------------------------------
    // ServerModel — a faithful native model of
    // runtime/src/cajeta/net/ServerModel.cajeta: the immutable carrier
    // (kind + poolSize) the builder's .model(...) step takes, with the two
    // factories (fiberPerConnection / sharedPool) and the same pool-size
    // floor-at-1 normalization.
    // -----------------------------------------------------------------------
    struct ServerModel {
        int32_t kind;
        int32_t poolSize;

        // ServerModel.fiberPerConnection(): Model A, poolSize unused (0).
        static ServerModel fiberPerConnection() {
            return ServerModel{FIBER_PER_CONNECTION_KIND, 0};
        }

        // ServerModel.sharedPool(n): Model B; a non-positive n floors to 1
        // (matching SharedPoolServer's ctor floor) so a model never names a
        // zero-worker pool.
        static ServerModel sharedPool(int32_t poolSize) {
            int32_t n = poolSize < 1 ? 1 : poolSize;
            return ServerModel{SHARED_POOL_KIND, n};
        }

        bool isFiberPerConnection() const { return kind == FIBER_PER_CONNECTION_KIND; }
        bool isSharedPool()         const { return kind == SHARED_POOL_KIND; }
    };

    // -----------------------------------------------------------------------
    // BuiltServer — the record of the factory call ServerBuilder.build() would
    // make: which concrete server (kind), the pool size threaded to a Model-B
    // bind, whether the backlog factory was used + the backlog value, and the
    // resolved bind address. Standing in for the live Server / SharedPoolServer
    // a real build returns, since constructing those needs real sockets.
    // -----------------------------------------------------------------------
    struct BuiltServer {
        int32_t     kind;         // FIBER_PER_CONNECTION_KIND | SHARED_POOL_KIND
        int32_t     poolSize;     // worker count for Model B; 0 for Model A
        bool        withBacklog;  // true ⇒ a bindWithBacklog factory was used
        int32_t     backlog;      // the listen backlog (only if withBacklog)
        std::string address;      // the bound address string
        bool        hasHandler;   // a handler was supplied
    };

    // -----------------------------------------------------------------------
    // ServerBuilder — a faithful native model of
    // runtime/src/cajeta/net/ServerBuilder.cajeta: accumulates address +
    // model + handler + backlog, defaulting the model to fiber-per-connection,
    // and build() dispatches on the model's kind to the matching factory
    // (delegating to ServerModel.bindServer{,WithBacklog}). No real sockets —
    // build() returns the BuiltServer *record* of the call it would make.
    // -----------------------------------------------------------------------
    class ServerBuilder {
    public:
        static constexpr int32_t UNSET_BACKLOG = 0;

        // Server.builder(): default model = fiber-per-connection, no address,
        // no handler, platform-default backlog.
        ServerBuilder()
            : address_(""), hasAddress_(false),
              model_(ServerModel::fiberPerConnection()),
              hasHandler_(false), backlog_(UNSET_BACKLOG) {}

        ServerBuilder& bind(const std::string& addr) {
            // (the real builder parses via SocketAddress.parse — here we just
            // record the string; parse validity is SocketAddress's own test.)
            address_ = addr;
            hasAddress_ = true;
            return *this;
        }

        // .model(m): a "null" selection is ignored — modeled here by the
        // explicit two-arg form below; the normal overload always sets.
        ServerBuilder& model(const ServerModel& m) {
            model_ = m;
            return *this;
        }

        // .model(null): the ignore-null path — the previously-set model stays.
        ServerBuilder& modelNull() {
            // intentionally a no-op: mirrors `if (m != null) { this.model = m }`
            return *this;
        }

        ServerBuilder& handler() {
            hasHandler_ = true;
            return *this;
        }

        // .backlog(n): a value < 1 resets to the platform default (UNSET).
        ServerBuilder& backlog(int32_t n) {
            backlog_ = n < 1 ? UNSET_BACKLOG : n;
            return *this;
        }

        // build(): the model-selection seam. Requires an address; dispatches
        // on the model's kind, threading poolSize for Model B and routing
        // through the bindWithBacklog factory iff an explicit backlog was set.
        BuiltServer build() const {
            if (!hasAddress_) {
                throw std::invalid_argument(
                    "Server.builder(): no bind address — call .bind(addr) first");
            }
            const bool withBacklog = backlog_ >= 1;
            BuiltServer out;
            out.kind        = model_.kind;
            out.poolSize    = model_.isSharedPool() ? model_.poolSize : 0;
            out.withBacklog = withBacklog;
            out.backlog     = withBacklog ? backlog_ : UNSET_BACKLOG;
            out.address     = address_;
            out.hasHandler  = hasHandler_;
            return out;
        }

        const ServerModel& selectedModel() const { return model_; }

    private:
        std::string address_;
        bool        hasAddress_;
        ServerModel model_;
        bool        hasHandler_;
        int32_t     backlog_;
    };

} // namespace cajeta::net::testing
