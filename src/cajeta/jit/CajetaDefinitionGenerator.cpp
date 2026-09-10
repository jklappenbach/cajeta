// The ORC definition generator that serves lazily-emitted cajeta bodies.

#include "cajeta/jit/CajetaDefinitionGenerator.h"

#include "cajeta/jit/CajetaLazyEmitter.h"
#include "cajeta/type/CajetaClass.h"
#include "cajeta/jit/LazyCodegen.h"

#include "llvm/Support/Error.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace cajeta {

    namespace {
        // Depth, not a flag: re-entry must unwind exactly as far as it nested.
        thread_local size_t g_gateDepth = 0;
    } // namespace

    CompilerGate& CompilerGate::instance() {
        static CompilerGate gate;
        return gate;
    }

    bool CompilerGate::heldByThisThread() { return g_gateDepth > 0; }

    void CompilerGate::observe(const std::function<void()>& fn) {
        struct Scope {
            CompilerGate& g;
            explicit Scope(CompilerGate& g) : g(g) {
                // Count THREADS, not entries — same-thread re-entry stays 1.
                if (g_gateDepth++ == 0) {
                    size_t now = ++g.inside;
                    size_t prev = g.maxInside.load(std::memory_order_relaxed);
                    while (now > prev
                           && !g.maxInside.compare_exchange_weak(prev, now)) {}
                }
            }
            ~Scope() {
                if (--g_gateDepth == 0) --g.inside;
            }
        } scope{*this};
        fn();
    }

    void CompilerGate::run(const std::function<void()>& fn) {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        observe(fn);
    }

    void CompilerGate::runUngatedForTest(const std::function<void()>& fn) {
        observe(fn);
    }

    std::vector<MethodPtr> CajetaDefinitionGenerator::resolve(
            const std::vector<std::string>& symbols) const {
        std::vector<MethodPtr> claimed;
        if (!lazyCodegenEnabled()) return claimed;
        for (const auto& symbol : symbols) {
            if (symbol.empty()) continue;
            if (auto method = index.find(symbol)) claimed.push_back(method);
        }
        return claimed;
    }

    llvm::Error CajetaDefinitionGenerator::tryToGenerate(
            llvm::orc::LookupState&, llvm::orc::LookupKind,
            llvm::orc::JITDylib& jd, llvm::orc::JITDylibLookupFlags,
            const llvm::orc::SymbolLookupSet& lookupSet) {
        if (!lazyCodegenEnabled()) return llvm::Error::success();

        std::vector<std::string> symbols;
        symbols.reserve(lookupSet.size());
        for (const auto& entry : lookupSet) {
            symbols.push_back((*entry.first).str());
        }

        // The lambda assigns over `result`, and that asserts unless it is checked.
        llvm::Error result = llvm::Error::success();
        llvm::ErrorAsOutParameter guard(&result);
        CompilerGate::instance().run([&] {
            // With no deliverer, fall through rather than fail the lookup.
            if (!deliver) return;
            auto t0 = std::chrono::steady_clock::now();
            size_t claimed = 0;
            // The claim chain for one symbol; a miss gets a SECOND attempt below.
            auto claim = [&](const std::string& symbol)
                    -> std::optional<
                           llvm::Expected<llvm::orc::ThreadSafeModule>> {
                if (auto method = index.find(symbol)) {
                    return emitMethodModule(method);
                }
                if (auto* thunk = index.findReflectThunk(symbol)) {
                    if (thunk->isInvoke) thunk->klass->emitReflectInvokeBody();
                    else thunk->klass->emitReflectNewBody();
                    auto* gv = index.findLiveDefinition(symbol);
                    if (!gv) {
                        return llvm::Expected<llvm::orc::ThreadSafeModule>(
                            llvm::createStringError(
                                llvm::inconvertibleErrorCode(),
                                "lazy emit: reflect emitter left no body "
                                "for '%s'", symbol.c_str()));
                    }
                    return snapshotLiveDefinition(gv);
                }
                if (auto* gv = index.findLiveDefinition(symbol)) {
                    return snapshotLiveDefinition(gv);
                }
                // Emulated TLS: a thread_local X reaches the object level only as
                // __emutls_v.X and __emutls_t.X, and one IR definition lowers to
                // both, so the sibling name must not deliver a second copy.
                for (const char* pre : {"__emutls_v.", "__emutls_t."}) {
                    if (symbol.rfind(pre, 0) != 0) continue;
                    std::string base = symbol.substr(11);
                    if (auto* gv = index.findLiveDefinition(base)) {
                        if (!servedEmutls.insert(base).second)
                            return std::nullopt;   // sibling already served
                        return snapshotLiveDefinition(gv);
                    }
                }
                return std::nullopt;
            };
            for (const auto& symbol : symbols) {
                if (symbol.empty()) continue;
                auto attempt = claim(symbol);
                if (!attempt) {
                    index.refresh();
                    attempt = claim(symbol);
                }
                if (!attempt) continue;   // not ours — ordinary fall-through
                llvm::Expected<llvm::orc::ThreadSafeModule> tsm =
                    std::move(*attempt);
                if (!tsm) {
                    result = tsm.takeError();
                    return;
                }
                if (llvm::Error err = deliver(std::move(*tsm), jd)) {
                    result = std::move(err);
                    return;
                }
                if (std::getenv("CAJETA_PRIME_TIMING")) {
                    std::fprintf(stderr, "[lazy]   -> %s\n", symbol.c_str());
                }
                ++claimed;
                ++generated;
            }
            if (claimed == 0) return;
            emitNs += std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (std::getenv("CAJETA_PRIME_TIMING")) {
                std::fprintf(stderr,
                             "[lazy] generated %zu body(ies); %zu total, "
                             "%lld ms\n",
                             claimed, generated, emitNs / 1000000);
            }
        });
        return result;
    }

} // namespace cajeta
