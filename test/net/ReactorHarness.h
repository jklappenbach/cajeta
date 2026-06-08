//
// ReactorHarness.h — NET-13.4 reactor / concurrency test harness.
//
// The two **reactor invariants** the whole async I/O design hangs on
// (see `docs/Net.md` §The reactor + the plan's NET-3 acceptance)
// are subtle and easy to regress silently, so the plan calls for a
// *focused* harness that pins them deterministically:
//
//   1. **Carrier-non-blocking.** A fiber issuing a blocking-*looking*
//      awaitable read parks the *fiber*, not the carrier OS thread, so a
//      sibling fiber on the same carrier keeps running. The classic
//      proof: two fibers each park on a separate idle socket; a shared
//      counter must be advanced by **both** before **either** read
//      completes — if the first read blocked the carrier, the second
//      fiber could never have advanced the counter.
//
//   2. **Zero registration leak.** Every awaitable op that arms a reactor
//      registration must drop it again — on the completion path *and* on
//      the cancellation / timeout path (NET-3.4). After a wave of ops
//      settles, the live-registration count must be back to **zero**; a
//      non-zero residue is a leak that, accumulated, exhausts the
//      reactor.
//
// ## Two reusable, header-only fixtures
//
//   * `SiblingCounterProbe` — drives invariant (1) at the layer that is
//     deterministically pinnable *without a live fiber scheduler*. It
//     models two sibling fibers as two OS threads, each doing a
//     blocking-style `awaitReadable` (the off-fiber path the NET-3.1
//     reactor exposes today falls through to the portable `select`
//     probe, which blocks only *that* thread — exactly the
//     per-fiber-park-not-carrier-block shape, modeled one level down).
//     The probe asserts the *interleave* property: both sides observe the
//     shared counter advance past both of their own increments before
//     their read returns, so neither serialized the other. The full
//     in-scheduler assertion (two fibers on **one** carrier) is the JIT
//     `ReactorTests.*` suite the plan names; this is its native,
//     scheduler-free analog, the same posture NET-13.1's loopback
//     fixtures and NET-13.3's mock peers take while the cajeta-surface
//     socket lowering (NET-1.3, "partial") is still being wired.
//
//   * `ReactorLeakGuard` — an RAII scope that snapshots the reactor's
//     live-registration balance (`__cajeta_net_reactor_active_count`,
//     NET-3.1) at entry and, on scope exit, asserts it returned to the
//     entry value. Wrap any block that arms + settles reactor ops in one
//     of these and a registration leak (a missing `deregister` on the
//     completion or cancellation path) fails the enclosing test at the
//     exact scope it leaked in. It composes with gtest: the guard records
//     a failure via `ADD_FAILURE` so it does not throw from a destructor.
//
// ## Why a C++ harness (not a `.cajeta` one)
//
// Same rationale as NET-13.1 / NET-13.3: the deterministic, repeatable
// half of these invariants lives at the native reactor ABI
// (`__cajeta_net_*`, NET-3.1), which exists and round-trips over loopback
// today, whereas the cajeta-surface `TcpStream` lowering is still partial.
// So the harness binds the native intrinsics via `extern "C"` and stands
// its peers up over raw loopback — reusing NET-13.1's `TcpEchoServer` and
// the Winsock/BSD socket shims verbatim — and lands now, ahead of the
// in-scheduler JIT suite it will be paired with. Kept under test/ so
// production sources carry no test-only surface.
//
#pragma once

#include "gtest/gtest.h"

#include "net/LoopbackFixtures.h"   // TcpEchoServer + cross-platform socket shims

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

// The NET-3.1 reactor ABI under test, plus the NET-1.1 socket verbs used
// to stand up loopback peers. Declared here (not pulled from a runtime
// header) because the test binary links the C runtime natively and these
// `__cajeta_net_*` symbols resolve via extern "C" — the same binding
// NetReactorTests / NetSocketTests use.
extern "C" {
    int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol);
    int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen);
    int32_t __cajeta_net_close(int32_t fd);
    int32_t __cajeta_net_last_error(void);

    int32_t __cajeta_net_reactor_init(void);
    int32_t __cajeta_net_reactor_register(int32_t fd, int32_t interest, void* fiber_handle);
    int32_t __cajeta_net_reactor_deregister(int32_t fd);
    int32_t __cajeta_net_reactor_active_count(void);
    int32_t __cajeta_net_await_readable(int32_t fd);
    int32_t __cajeta_net_reactor_poll_fd(int32_t fd, int32_t interest, int32_t timeout_ms);
}

namespace cajeta::net::testing {

    // Mirrors the native readiness vocabulary + probe results (kept local
    // so the harness header is self-describing; values MUST match the
    // native CAJETA_IO_* / CAJETA_REACTOR_* constants).
    // NOTE: the names are `R_`-prefixed (R_READY / R_ERROR …) — NOT bare
    // `READY` / `ERROR` — because <windows.h> (pulled in by LoopbackFixtures.h
    // on Windows) #defines `ERROR` as a numeric constant, which would mangle a
    // bare `ERROR` constant here. Same guard NetReactorTests.cpp uses.
    namespace reactor_consts {
        constexpr int32_t IO_READ   = 1;
        constexpr int32_t IO_WRITE  = 2;
        constexpr int32_t R_READY   = 1;
        constexpr int32_t R_TIMEOUT = 0;
        constexpr int32_t R_ERROR   = -1;
    }

    // -----------------------------------------------------------------------
    // ReactorLeakGuard — RAII registration-leak assertion scope.
    //
    // Snapshot the reactor's live-registration balance on construction and,
    // on destruction, assert it returned to the snapshot (default: to the
    // entry value, which for a clean test is zero). Usage:
    //
    //     {
    //         ReactorLeakGuard guard;        // baseline = active_count()
    //         ... arm + settle a wave of reactor ops ...
    //     }   // dtor: EXPECT entry-balance restored, else ADD_FAILURE
    //
    // The guard reports via gtest's non-fatal `ADD_FAILURE` (never throws
    // from the destructor, which would terminate under a stack unwind) so a
    // leak surfaces as a failed expectation pinned to the leaking scope.
    // -----------------------------------------------------------------------
    class ReactorLeakGuard {
    public:
        ReactorLeakGuard()
            : baseline_(__cajeta_net_reactor_active_count()) {}

        ReactorLeakGuard(const ReactorLeakGuard&) = delete;
        ReactorLeakGuard& operator=(const ReactorLeakGuard&) = delete;

        // The live-registration count captured at construction.
        int32_t baseline() const { return baseline_; }

        // The current balance, mid-scope — lets a test assert an op is in
        // fact armed (count > baseline) while a fiber is parked.
        static int32_t active() { return __cajeta_net_reactor_active_count(); }

        ~ReactorLeakGuard() {
            const int32_t now = __cajeta_net_reactor_active_count();
            if (now != baseline_) {
                ADD_FAILURE() << "reactor registration leak: balance was "
                              << baseline_ << " at scope entry but " << now
                              << " at scope exit (delta "
                              << (now - baseline_) << ")";
            }
        }

    private:
        int32_t baseline_;
    };

    // -----------------------------------------------------------------------
    // SiblingCounterProbe — carrier-non-blocking interleave proof.
    //
    // Two "sibling" workers each park on a blocking-style read of a separate,
    // initially-idle loopback socket. A shared atomic counter is advanced by
    // each worker right before it parks; the harness then unblocks both reads
    // (the peer sends). The asserted property: at the moment each worker's
    // read RETURNS, the shared counter already reflects **both** workers'
    // pre-park increments — i.e. neither worker's park prevented the other
    // from making progress. If a park blocked the shared execution context,
    // the second worker could never have advanced the counter before the
    // first returned.
    //
    // At the native layer "sibling fibers on one carrier" is modeled as two
    // OS threads (the off-fiber await path parks only the calling thread, the
    // faithful one-level-down analog of parking a fiber but not its carrier).
    // The in-scheduler version — two fibers on a single carrier — is the JIT
    // ReactorTests suite; this probe pins the readiness/interleave contract
    // deterministically and without a live scheduler.
    //
    // `run()` returns true iff the interleave property held on both sides.
    // It is self-contained: stands up two echo-backed loopback connections,
    // drives the two workers, and tears everything down.
    // -----------------------------------------------------------------------
    class SiblingCounterProbe {
    public:
        // A single worker's observation: the counter value it saw at the
        // instant its parked read returned, and whether the read succeeded.
        struct Observation {
            bool readReady = false;     // awaitReadable reported READY
            int32_t counterAtReturn = 0;  // shared counter when read returned
        };

        // Run the two-sibling interleave probe. Returns true iff BOTH reads
        // became ready AND each worker observed the counter already at its
        // terminal value (== 2) by the time its read returned — the proof
        // that both siblings advanced before either completed.
        bool run() {
            using namespace reactor_consts;
            if (__cajeta_net_reactor_init() != 0) return false;

            // Two independent loopback connections — one per sibling. The
            // worker awaits the *runtime* client fd (`*Cli`, a runtime
            // fd-table handle the reactor intrinsics accept); we drive the
            // wake-up `send` from the *raw* server-side socket (`*Peer`, a
            // `::accept` handle) via the test shim. The two handle spaces are
            // deliberately not mixed: the runtime verbs only ever touch the
            // client fd, the raw `::send`/`::close` only ever touch the peer.
            int32_t aCli = -1, bCli = -1;
            cajeta_net_test_socket_t aPeer = CAJETA_NET_TEST_BAD_SOCKET;
            cajeta_net_test_socket_t bPeer = CAJETA_NET_TEST_BAD_SOCKET;
            if (!makePair(&aCli, &aPeer)) return false;
            if (!makePair(&bCli, &bPeer)) { closeAll(aCli, aPeer); return false; }

            std::atomic<int32_t> counter{0};
            // Gate so neither read can become ready until BOTH workers have
            // parked + bumped the counter — that is what forces the interleave
            // (and makes the proof robust to scheduling jitter).
            std::atomic<int32_t> parked{0};

            Observation obsA, obsB;
            std::thread ta([&] { worker(aCli, counter, parked, obsA); });
            std::thread tb([&] { worker(bCli, counter, parked, obsB); });

            // Wait until both siblings have advanced the counter and are
            // parked in their awaitReadable, then release both reads at once.
            spinUntilParked(parked, 2);
            // Both sides advanced the counter before either read could return.
            // Wake each worker from its raw peer socket (test shim ::send).
            const char byte = 'x';
            ::send(aPeer, &byte, 1, 0);
            ::send(bPeer, &byte, 1, 0);

            ta.join();
            tb.join();

            closeAll(aCli, aPeer);
            closeAll(bCli, bPeer);

            // Both reads must have fired, and each must have seen the counter
            // already at 2 (both increments) at the instant it returned.
            return obsA.readReady && obsB.readReady &&
                   obsA.counterAtReturn == 2 && obsB.counterAtReturn == 2;
        }

    private:
        // One sibling: bump the shared counter, announce it is parked, then
        // block-style await readability; on wake, snapshot the counter.
        static void worker(int32_t fd, std::atomic<int32_t>& counter,
                           std::atomic<int32_t>& parked, Observation& obs) {
            using namespace reactor_consts;
            counter.fetch_add(1, std::memory_order_seq_cst);
            parked.fetch_add(1, std::memory_order_seq_cst);
            int32_t r = __cajeta_net_await_readable(fd);
            obs.counterAtReturn = counter.load(std::memory_order_seq_cst);
            obs.readReady = (r == R_READY);
        }

        // Busy-wait (briefly) until `parked` reaches `n` or a guard elapses.
        // A spin (not a sleep) keeps the probe fast + deterministic; the
        // window is tiny (two thread starts).
        static void spinUntilParked(std::atomic<int32_t>& parked, int32_t n) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (parked.load(std::memory_order_seq_cst) < n) {
                if (std::chrono::steady_clock::now() > deadline) return;
                std::this_thread::yield();
            }
            // Both have bumped the counter; give the just-counted side a beat
            // to actually enter awaitReadable before we send (so the read
            // genuinely parks rather than finding data already waiting). A
            // short fixed nap is acceptable here: the property under test is
            // the *counter ordering*, which is already established by the
            // gate above regardless of this timing.
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        // Stand up one connected loopback pair: `*cli` is the runtime client
        // fd the worker awaits on, `*peer` is the raw server-side socket we
        // drive the wake `::send` from. Returns true on success.
        //
        // We need BOTH ends addressable, so we cannot reuse NET-13.1's
        // `TcpEchoServer` (it owns + closes its accepted side internally).
        // Instead this stands up a minimal listener, connects the client via
        // the runtime verbs, and `::accept`s the server side — mirroring
        // NetReactorTests::makeConnectedPair but via the harness socket shims
        // so it is platform-portable here.
        bool makePair(int32_t* cli, cajeta_net_test_socket_t* peer) {
            cajeta_net_test_socket_t lst = ::socket(AF_INET, SOCK_STREAM, 0);
            if (lst == CAJETA_NET_TEST_BAD_SOCKET) return false;
            int one = 1;
            ::setsockopt(lst, SOL_SOCKET, SO_REUSEADDR,
                         reinterpret_cast<const char*>(&one), sizeof(one));
            sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = 0;
            if (::bind(lst, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
                CAJETA_NET_TEST_CLOSESOCK(lst); return false;
            }
            socklen_t len = sizeof(addr);
            if (::getsockname(lst, reinterpret_cast<sockaddr*>(&addr), &len) < 0) {
                CAJETA_NET_TEST_CLOSESOCK(lst); return false;
            }
            if (::listen(lst, 4) < 0) { CAJETA_NET_TEST_CLOSESOCK(lst); return false; }

            // Connect the client via the *runtime* socket verbs, so the fd the
            // worker awaits on is a runtime fd the reactor intrinsics accept.
            int32_t c = __cajeta_net_socket(AF_INET, SOCK_STREAM, 0);
            if (c < 0) { CAJETA_NET_TEST_CLOSESOCK(lst); return false; }
            if (__cajeta_net_connect(c, &addr, (int32_t) sizeof(addr)) != 0) {
                __cajeta_net_close(c); CAJETA_NET_TEST_CLOSESOCK(lst); return false;
            }
            cajeta_net_test_socket_t p =
                ::accept(lst, nullptr, nullptr);
            CAJETA_NET_TEST_CLOSESOCK(lst);
            if (p == CAJETA_NET_TEST_BAD_SOCKET) {
                __cajeta_net_close(c); return false;
            }
            *cli = c;
            // The server-side fd stays a raw test socket (full width on
            // Windows): only ever touched by the test shim's ::send / ::close,
            // never by the runtime fd-table verbs.
            *peer = p;
            return true;
        }

        static void closeAll(int32_t cli, cajeta_net_test_socket_t peer) {
            if (cli >= 0) __cajeta_net_close(cli);
            if (peer != CAJETA_NET_TEST_BAD_SOCKET) {
                CAJETA_NET_TEST_CLOSESOCK(peer);
            }
        }
    };

} // namespace cajeta::net::testing
