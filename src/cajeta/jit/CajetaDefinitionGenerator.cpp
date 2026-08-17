#include "cajeta/jit/CajetaDefinitionGenerator.h"

#include "cajeta/jit/LazyCodegen.h"

#include <cstdio>
#include <cstdlib>

namespace cajeta {

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

        auto claimed = resolve(symbols);
        if (claimed.empty()) return llvm::Error::success();
        // Without a host emitter the generator can decide but not deliver.
        // Returning success leaves the lookup to fall through exactly as it
        // does today rather than failing it — Unit 2 must not be able to break
        // a session it is not yet driving.
        if (!emit) return llvm::Error::success();

        for (const auto& method : claimed) {
            if (auto err = emit(method, jd)) return err;
            ++generated;
        }
        if (std::getenv("CAJETA_PRIME_TIMING")) {
            std::fprintf(stderr, "[lazy] generated %zu body(ies), %zu total\n",
                         claimed.size(), generated);
        }
        return llvm::Error::success();
    }

} // namespace cajeta
